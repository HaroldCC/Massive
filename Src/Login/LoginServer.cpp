/**
 * @file LoginServer.cpp
 * @brief LoginServer 实现——完整认证主逻辑
 *
 * 认证链路（3 线程参与）：
 *   IO 线程: RateLimiter.Allow → DB::Range<AccountsTable>().SingleOrDefault → return
 *   主线程: DB 回调 → argon2id 验密码 → 封禁检查 → ECDH → SessionToken 签发 → asio::post
 *   IO 线程: asio::post 回调 → LoginAuthRsp 序列化 → Send → Close
 */
#include "Login/LoginServer.h"
#include "Login/LoginConfig.h"
#include "Login/RateLimiter.h"

#include "Common/Crypto/Argon2id.h"
#include "Common/Crypto/SessionToken.h"
#include "Common/DB/AutoGen/AccountsTable.gen.h"
#include "Common/DB/DBWorkerPool.h"
#include "Common/DB/Range.h"
#include "Common/DB/Types.h"
#include "Common/Log/Log.h"
#include "Common/Network/TCPSocket.h"

#include <Login.pb.h>
#include <MsgID.pb.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <thread>

using namespace std::chrono_literals;

namespace MMO
{

    // ── LoginServer 生命周期 ──

    bool LoginServer::Init(const LoginConfig &cfg)
    {
        // 拷贝核心配置
        std::memcpy(_lss, cfg.security.loginServerSecret, LoginConfig::kLSSSize);
        _worldServerID = cfg.world.worldServerID;
        _gateIPs       = cfg.world.gateIPs;

        _ioPool   = std::make_unique<IOContextPool>(static_cast<size_t>(cfg.network.ioThreads));
        _acceptor = std::make_unique<TCPAcceptor>(*_ioPool, cfg.network.port);

        // 注册 MSG_LOGIN_AUTH_REQ handler（IO 线程）
        _dispatcher.Register<Proto::LoginAuthReq>(
            Proto::MSG_LOGIN_AUTH_REQ,
            [this](std::shared_ptr<TCPSocket> socket, const Proto::LoginAuthReq &req) {
                std::string clientIP = "0.0.0.0";
                try
                {
                    clientIP = socket->LowestLayer().remote_endpoint().address().to_string();
                }
                catch (...)
                {
                }
                HandleLoginAuthReq(std::move(socket), req, clientIP);
            });

        // acceptor 回调：设置 MessageHandler → Start
        _acceptor->Start([this](std::shared_ptr<TCPSocket> socket) {
            auto sockPtr = socket.get();
            socket->SetMessageHandler([this, sock = std::move(socket)](uint32 msgID,
                                                                       uint32 /*sessionID*/,
                                                                       const uint8 *body,
                                                                       size_t       len) mutable {
                _dispatcher.Dispatch(sock, msgID, body, len);
            });
            sockPtr->Start();
        });

        Log::Info("LoginServer listening on port {}", cfg.network.port);
        return true;
    }

    void LoginServer::Run()
    {
        _running.store(true, std::memory_order_release);
        _ioPool->Start();

        // 主线程轮询 DB 回调
        while (_running.load(std::memory_order_acquire))
        {
            DB::DBWorkerPool::Instance().ProcessCallbacks();
            std::this_thread::sleep_for(1ms);
        }
    }

    void LoginServer::Stop()
    {
        _running.store(false, std::memory_order_release);
        if (_acceptor)
        {
            _acceptor->Stop();
        }
        if (_ioPool)
        {
            _ioPool->Stop();
        }
    }

    // ── Step 1: IO 线程 — 限流检查 + 发起 DB 异步查询 ──

    void LoginServer::HandleLoginAuthReq(std::shared_ptr<TCPSocket> socket,
                                         const Proto::LoginAuthReq &req,
                                         const std::string         &clientIP)
    {
        // 1. RateLimiter 检查
        if (!_rateLimiter.Allow(clientIP))
        {
            Log::Debug("LoginServer: IP {} rate limited, closing silently", clientIP);
            socket->Close();
            return;
        }

        Log::Info("LoginServer auth request from {} ({})", req.username(), clientIP);

        // 2. 类型安全查询（Range<AccountsTable>），只读取认证所需的列
        std::string username    = req.username();
        std::string password    = req.password();
        std::string clientDHKey = req.client_dh_key();

        DB::Range<DB::AutoGen::AccountsTable>()
            .Select(DB::AutoGen::AccountsTable::account_id,
                    DB::AutoGen::AccountsTable::password_hash,
                    DB::AutoGen::AccountsTable::ban_until)
            .Where(DB::AutoGen::AccountsTable::username == username)
            .SingleOrDefault(
                [this,
                 socket = std::move(socket),
                 clientIP,
                 username    = std::move(username),
                 password    = std::move(password),
                 clientDHKey = std::move(clientDHKey)](std::optional<DB::AutoGen::accounts_row> row) mutable {
                    OnAuthDBCallback(std::move(socket),
                                     std::move(clientIP),
                                     std::move(username),
                                     std::move(password),
                                     std::move(clientDHKey),
                                     std::move(row));
                });
    }

    // ── Step 2: 主线程 — DB 回调: 验密码 + ECDH + SessionToken 签发 ──

    void LoginServer::OnAuthDBCallback(std::shared_ptr<TCPSocket>              socket,
                                       std::string                             clientIP,
                                       const std::string                      &username,
                                       const std::string                      &password,
                                       const std::string                      &clientDHKey,
                                       std::optional<DB::AutoGen::accounts_row> row)
    {
        // --- 用户不存在 ---
        if (!row.has_value())
        {
            Log::Debug("LoginServer: auth failed for '{}' — user not found", username);
            _rateLimiter.RecordFailure(clientIP);

            asio::post(socket->LowestLayer().get_executor(),
                       [this, socket, errorCode = static_cast<uint32>(1001)]() {
                           SendAuthFailure(socket, errorCode);
                       });
            return;
        }

        // --- argon2id 密码验证 ---
        if (!Crypto::Argon2id::VerifyPassword(password, row->password_hash))
        {
            Log::Debug("LoginServer: auth failed for '{}' — wrong password", username);
            _rateLimiter.RecordFailure(clientIP);

            asio::post(socket->LowestLayer().get_executor(),
                       [this, socket, errorCode = static_cast<uint32>(1001)]() {
                           SendAuthFailure(socket, errorCode);
                       });
            return;
        }

        // --- 封禁检查 ---
        if (row->ban_until.unix_ms > 0)
        {
            auto now = DB::Timestamp::Now();
            if (row->ban_until.unix_ms > now.unix_ms)
            {
                Log::Debug("LoginServer: auth failed for '{}' — account banned until {}",
                           username,
                           row->ban_until.ToPGText());
                _rateLimiter.RecordFailure(clientIP);

                asio::post(socket->LowestLayer().get_executor(),
                           [this, socket, errorCode = static_cast<uint32>(1002)]() {
                               SendAuthFailure(socket, errorCode);
                           });
                return;
            }
        }

        int32 accountID = row->account_id;

        // --- ECDH 密钥协商 ---
        auto keyPairOpt = Crypto::EcdhX25519::GenerateKeyPair();
        if (!keyPairOpt)
        {
            Log::Error("LoginServer: ECDH key generation failed for '{}'", username);
            asio::post(socket->LowestLayer().get_executor(),
                       [this, socket, errorCode = static_cast<uint32>(1003)]() {
                           SendAuthFailure(socket, errorCode);
                       });
            return;
        }
        auto &keyPair = *keyPairOpt;

        // client_dh_key 是 bytes 字段 (protobuf)，原始 32B X25519 公钥
        if (clientDHKey.size() != Crypto::EcdhX25519::kKeySize)
        {
            Log::Error("LoginServer: client DH key size mismatch (got {}, expected {})",
                       clientDHKey.size(),
                       Crypto::EcdhX25519::kKeySize);
            asio::post(socket->LowestLayer().get_executor(),
                       [this, socket, errorCode = static_cast<uint32>(1003)]() {
                           SendAuthFailure(socket, errorCode);
                       });
            return;
        }

        auto sharedSecretOpt =
            Crypto::EcdhX25519::DeriveSharedSecret(keyPair.privateKey.Data(),
                                                   reinterpret_cast<const uint8 *>(clientDHKey.data()));
        if (!sharedSecretOpt)
        {
            Log::Error("LoginServer: ECDH shared secret derivation failed for '{}'", username);
            asio::post(socket->LowestLayer().get_executor(),
                       [this, socket, errorCode = static_cast<uint32>(1003)]() {
                           SendAuthFailure(socket, errorCode);
                       });
            return;
        }

        // sharedSecret → SHA-256 → 32B SessionKey
        auto sessionKeyBuf = Crypto::EcdhX25519::DeriveSessionKey(sharedSecretOpt->Data());

        // --- SessionToken 签发 ---
        auto now      = static_cast<uint32>(std::time(nullptr));
        auto tokenOpt = Crypto::SessionTokenBuilder::Issue(_lss,
                                                           sessionKeyBuf.Data(),
                                                           _worldServerID,
                                                           static_cast<uint32>(accountID),
                                                           now + kTokenExpireSec);
        if (!tokenOpt)
        {
            Log::Error("LoginServer: SessionToken issue failed for '{}'", username);
            asio::post(socket->LowestLayer().get_executor(),
                       [this, socket, errorCode = static_cast<uint32>(1003)]() {
                           SendAuthFailure(socket, errorCode);
                       });
            return;
        }

        // --- asio::post 回到 IO 线程，发送成功响应 ---
        Log::Info("LoginServer: auth success for '{}' (accountID={})", username, accountID);

        asio::post(socket->LowestLayer().get_executor(),
                   [this, socket, keyPair = std::move(keyPair), token = std::move(*tokenOpt)]() mutable {
                       SendAuthSuccess(socket, keyPair, token);
                   });
    }

    // ── Step 3: IO 线程 — 认证成功 → LoginAuthRsp + Send + Close ──

    void LoginServer::SendAuthSuccess(std::shared_ptr<TCPSocket>  socket,
                                      const Crypto::EcdhKeyPair  &keyPair,
                                      const Crypto::SessionToken &token)
    {
        Proto::LoginAuthRsp rsp;
        auto               *error = rsp.mutable_error();
        error->set_code(0);

        // gateIPs
        for (const auto &ip : _gateIPs)
        {
            rsp.add_gate_ips(ip);
        }

        // server DH public key (32B)
        rsp.set_server_dh_key(keyPair.publicKey.Data(), keyPair.publicKey.Size());

        // SessionToken (46B)
        rsp.set_session_token(token.data, Crypto::SessionToken::kTotalSize);

        auto rawData = rsp.SerializeAsString();
        auto rawBody = reinterpret_cast<const uint8 *>(rawData.data());

        // 构建 PacketHeader: [length:4B][msgID:4B][sessionID:4B][body]
        uint32     totalLen = static_cast<uint32>(sizeof(PacketHeader) + rawData.size());
        ByteBuffer frame    = ByteBuffer::Own(totalLen);
        frame.WriteUint32(totalLen);
        frame.WriteUint32(Proto::MSG_LOGIN_AUTH_RSP);
        frame.WriteUint32(0); // sessionID=0 (短连接)
        frame.WriteBytes(rawBody, rawData.size());

        // SendThenClose：写队列排空后自动关闭，避免 Send→立刻 Close 的时序 Bug
        socket->SendThenClose(std::move(frame));
    }

    // ── Step 4: IO 线程 — 认证失败 → 错误 LoginAuthRsp + Send + Close ──

    void LoginServer::SendAuthFailure(std::shared_ptr<TCPSocket> socket, uint32 errorCode)
    {
        Proto::LoginAuthRsp rsp;
        auto               *error = rsp.mutable_error();
        error->set_code(errorCode);

        switch (errorCode)
        {
            case 1001:
                error->set_message("Invalid credentials");
                break;
            case 1002:
                error->set_message("Account banned");
                break;
            case 1003:
                error->set_message("Internal server error");
                break;
            default:
                error->set_message("Unknown error");
                break;
        }

        auto rawData = rsp.SerializeAsString();
        auto rawBody = reinterpret_cast<const uint8 *>(rawData.data());

        // 构建 PacketHeader: [length:4B][msgID:4B][sessionID:4B][body]
        uint32     totalLen = static_cast<uint32>(sizeof(PacketHeader) + rawData.size());
        ByteBuffer frame    = ByteBuffer::Own(totalLen);
        frame.WriteUint32(totalLen);
        frame.WriteUint32(Proto::MSG_LOGIN_AUTH_RSP);
        frame.WriteUint32(0); // sessionID=0 (短连接)
        frame.WriteBytes(rawBody, rawData.size());

        socket->SendThenClose(std::move(frame));
    }

} // namespace MMO
