/**
 * @file VirtualClient.cpp
 * @brief VirtualClient 异步状态机实现
 */
#include <cstring>
#include <random>

#include "TestClient/VirtualClient.h"
#include "TestClient/Scenario.h"
#include "TestClient/StatsCollector.h"

#include "Common/Crypto/EcdhX25519.h"
#include "Common/Crypto/SessionToken.h"
#include "Common/Log/Log.h"
#include "Common/Network/IOContextPool.h"
#include "Common/Network/TCPSocket.h"

#include <Login.pb.h>
#include <MsgID.pb.h>
#include <Move.pb.h>

namespace MMO::TestClient
{

    VirtualClient::VirtualClient(uint32                 clientID,
                                std::string            username,
                                std::string            password,
                                std::string            loginHost,
                                uint16                 loginPort,
                                StatsCollector        &stats,
                                IOContextPool         &pool,
                                std::unique_ptr<Scenario> scenario)
        : _clientID(clientID)
        , _name(std::to_string(clientID))
        , _username(std::move(username))
        , _password(std::move(password))
        , _loginHost(std::move(loginHost))
        , _loginPort(loginPort)
        , _stats(stats)
        , _pool(pool)
        , _scenario(std::move(scenario))
        , _tickTimer(pool.GetNextContext())
    {
        if (_scenario)
        {
            _scenario->_owner = this;
        }

        // 生成客户端随机数（nonce 下半部分 8B）
        std::random_device rd;
        std::mt19937_64    rng(rd());
        _clientRandom = rng();

        _name = "client_" + std::to_string(clientID);
    }

    VirtualClient::~VirtualClient()
    {
        Disconnect();
    }

    // ── 启动 ──

    void VirtualClient::Start()
    {
        _state = ClientState::ConnectingLogin;

        auto &ctx    = _pool.GetNextContext();
        auto socket  = std::make_shared<TCPSocket>(asio::ip::tcp::socket(ctx), EFraming::PacketHeader);

        // 消息回调
        socket->SetMessageHandler([self = shared_from_this()](uint32 msgID, uint32 sessionID,
                                                              const uint8 *body, size_t len) {
            switch (self->_state)
            {
                case ClientState::SendingAuth:
                    self->OnAuthRsp(msgID, sessionID, body, len);
                    break;
                case ClientState::SendingEnterWorld:
                    self->OnEnterWorldRsp(msgID, sessionID, body, len);
                    break;
                case ClientState::InWorld:
                    self->OnGateMessage(msgID, sessionID, body, len);
                    break;
                default:
                    break;
            }
        });

        // 断线回调（Login 短连接预期认证完毕连接会断开，不视为失败）
        socket->SetCloseHandler([self = shared_from_this()]() {
            self->_stats.RecordDisconnect();
            // SendingAuth = 认证完毕 LoginServer 主动断开，是正常流程
            // ConncetingGate/SendingEnterWorld/InWorld = 登录 socket 已替换
            if (self->_state != ClientState::Disconnecting
                && self->_state != ClientState::Failed
                && self->_state != ClientState::SendingAuth)
            {
                self->EnterFailedState("connection closed");
            }
        });

        _socket = socket;

        // 解析地址
        asio::ip::tcp::resolver resolver(ctx);
        asio::error_code        resolveEc;
        auto endpoints = resolver.resolve(_loginHost, std::to_string(_loginPort), resolveEc);
        if (resolveEc || endpoints.empty())
        {
            EnterFailedState("resolve failed: " + _loginHost + ":" + std::to_string(_loginPort));
            return;
        }

        _socket->LowestLayer().async_connect(*endpoints.begin(),
            [self = shared_from_this()](const asio::error_code &ec) {
                self->OnLoginConnected(ec);
            });
    }

    void VirtualClient::OnLoginConnected(const asio::error_code &ec)
    {
        if (ec)
        {
            EnterFailedState("login connect: " + ec.message());
            return;
        }

        _socket->Start();
        SendAuthReq();
    }

    // ── Step 1: 发送 LoginAuthReq ──

    void VirtualClient::SendAuthReq()
    {
        _state = ClientState::SendingAuth;

        // ECDH: 生成密钥对
        auto keyPairOpt = Crypto::EcdhX25519::GenerateKeyPair();
        if (!keyPairOpt)
        {
            EnterFailedState("ECDH key generation failed");
            return;
        }
        auto &keyPair = *keyPairOpt;

        // 暂存私钥（稍后推导 SessionKey 用）
        ByteBuffer privateKey = std::move(keyPair.privateKey); // move (ByteBuffer is non-copyable)

        Proto::LoginAuthReq req;
        req.set_username(_username);
        req.set_password(_password);
        req.set_client_dh_key(keyPair.publicKey.Data(), keyPair.publicKey.Size());

        auto data = req.SerializeAsString();
        BuildAndSendPacket(Proto::MSG_LOGIN_AUTH_REQ,
                          reinterpret_cast<const uint8 *>(data.data()),
                          data.size());

        // 暂存私钥到成员变量（用 sessionKey 临时存，后续 ECDH 后覆盖）
        _sessionKey = std::move(privateKey);
    }

    // ── Step 2: 接收 LoginAuthRsp ──

    void VirtualClient::OnAuthRsp(uint32 msgID, uint32 /*sessionID*/, const uint8 *body, size_t len)
    {
        if (msgID != Proto::MSG_LOGIN_AUTH_RSP)
        {
            Log::Debug("[{}] unexpected msgID={} during auth", _name, msgID);
            return;
        }

        Proto::LoginAuthRsp rsp;
        if (!rsp.ParseFromArray(body, static_cast<int32>(len)))
        {
            EnterFailedState("LoginAuthRsp parse failed");
            return;
        }

        if (rsp.error().code() != 0)
        {
            _stats.RecordLoginAttempt(false, rsp.error().code());
            EnterFailedState("LoginAuthRsp error: " + rsp.error().message());
            return;
        }

        _stats.RecordLoginAttempt(true);

        // ECDH: 计算 shared secret → SessionKey
        auto sharedSecretOpt = Crypto::EcdhX25519::DeriveSharedSecret(
            _sessionKey.Data(),  // 之前存的私钥
            reinterpret_cast<const uint8 *>(rsp.server_dh_key().data()));
        if (!sharedSecretOpt)
        {
            EnterFailedState("ECDH shared secret derivation failed");
            return;
        }

        // SHA-256 → SessionKey
        _sessionKey = Crypto::EcdhX25519::DeriveSessionKey(sharedSecretOpt->Data());

        // 存储 SessionToken
        auto &tokenStr = rsp.session_token();
        _sessionToken  = ByteBuffer::Copy(
            reinterpret_cast<const uint8 *>(tokenStr.data()), tokenStr.size());

        // 解析 Gate IP 列表
        if (rsp.gate_ips_size() > 0)
        {
            ParseGateIPs(rsp.gate_ips(0));
        }
        else
        {
            EnterFailedState("LoginAuthRsp: no gate_ips");
            return;
        }

        // 关闭 Login 连接
        _socket->Close();
        _socket.reset();

        // 连接到 Gate
        ConnectToGate();
    }

    void VirtualClient::ParseGateIPs(const std::string &gateIP)
    {
        auto colon = gateIP.find(':');
        if (colon == std::string::npos)
        {
            _gateHost = gateIP;
            _gatePort = 9001; // 默认 Gate 端口
        }
        else
        {
            _gateHost = gateIP.substr(0, colon);
            _gatePort = static_cast<uint16>(std::stoi(gateIP.substr(colon + 1)));
        }
    }

    // ── Step 3: 连接 Gate ──

    void VirtualClient::ConnectToGate()
    {
        _state = ClientState::ConnectingGate;

        auto &ctx    = _pool.GetNextContext();
        auto socket = std::make_shared<TCPSocket>(asio::ip::tcp::socket(ctx), EFraming::PacketHeader);

        socket->SetMessageHandler([self = shared_from_this()](uint32 msgID, uint32 sessionID,
                                                              const uint8 *body, size_t len) {
            if (self->_state == ClientState::SendingEnterWorld)
            {
                self->OnEnterWorldRsp(msgID, sessionID, body, len);
            }
            else if (self->_state == ClientState::InWorld)
            {
                self->OnGateMessage(msgID, sessionID, body, len);
            }
        });

        socket->SetCloseHandler([self = shared_from_this()]() {
            self->_stats.RecordDisconnect();
            if (self->_state != ClientState::Disconnecting && self->_state != ClientState::Failed)
            {
                self->EnterFailedState("gate connection closed");
            }
        });

        _socket = socket;

        asio::ip::tcp::resolver resolver(ctx);
        asio::error_code        resolveEc;
        auto endpoints = resolver.resolve(_gateHost, std::to_string(_gatePort), resolveEc);
        if (resolveEc || endpoints.empty())
        {
            EnterFailedState("gate resolve failed: " + _gateHost + ":" + std::to_string(_gatePort));
            return;
        }

        _socket->LowestLayer().async_connect(*endpoints.begin(),
            [self = shared_from_this()](const asio::error_code &ec) {
                self->OnGateConnected(ec);
            });
    }

    void VirtualClient::OnGateConnected(const asio::error_code &ec)
    {
        if (ec)
        {
            EnterFailedState("gate connect: " + ec.message());
            return;
        }

        _socket->Start();
        SendEnterWorldReq();
    }

    // ── Step 4: 发送 EnterWorldReq ──

    void VirtualClient::SendEnterWorldReq()
    {
        _state = ClientState::SendingEnterWorld;

        Proto::LoginEnterWorldReq req;
        req.set_session_token(_sessionToken.Data(), _sessionToken.Size());
        req.set_nonce(_clientRandom);

        auto data = req.SerializeAsString();
        BuildAndSendPacket(Proto::MSG_LOGIN_ENTER_WORLD_REQ,
                          reinterpret_cast<const uint8 *>(data.data()),
                          data.size());
    }

    void VirtualClient::OnEnterWorldRsp(uint32 msgID, uint32 /*sessionID*/, const uint8 *body, size_t len)
    {
        if (msgID != Proto::MSG_LOGIN_ENTER_WORLD_RSP)
        {
            Log::Debug("[{}] unexpected msgID={} during enterworld", _name, msgID);
            return;
        }

        Proto::LoginEnterWorldRsp rsp;
        if (!rsp.ParseFromArray(body, static_cast<int32>(len)))
        {
            EnterFailedState("EnterWorldRsp parse failed");
            return;
        }

        if (rsp.error().code() != 0)
        {
            _stats.RecordEnterWorldAttempt(false, rsp.error().code());
            EnterFailedState("EnterWorldRsp error: " + rsp.error().message());
            return;
        }

        _stats.RecordEnterWorldAttempt(true);

        // 初始化 CryptoSession（用于后续 AES-GCM 加解密）
        _crypto.Init(_sessionKey.Data(), _clientRandom);

        _state = ClientState::InWorld;
        Log::Info("[{}] Entered World (playerID={}, sceneID={})",
                  _name, rsp.player_id(), rsp.scene_id());

        // 启动 Scenario
        if (_scenario)
        {
            _scenario->OnEnter();
        }

        // 启动 20ms Tick
        _lastTick = std::chrono::steady_clock::now();
        StartTick();
    }

    // ── Step 5: InWorld 消息循环 ──

    void VirtualClient::OnGateMessage(uint32 msgID, uint32 /*sessionID*/, const uint8 *body, size_t len)
    {
        switch (msgID)
        {
            case Proto::MSG_HEARTBEAT_RSP:
            {
                Proto::HeartbeatRsp rsp;
                if (rsp.ParseFromArray(body, static_cast<int32>(len)))
                {
                    _stats.RecordHeartbeatRcvd();
                }
                break;
            }
            case Proto::MSG_MOVE_RSP:
            {
                // 入站帧: [Seq:4B][Ciphertext+Tag]
                if (len <= sizeof(uint32))
                {
                    return;
                }
                uint32     seq     = ByteBuffer::Wrap(body, sizeof(uint32)).ReadUint32();
                size_t     encLen  = len - sizeof(uint32);
                const uint8 *encBody = body + sizeof(uint32);

                auto plaintext = _crypto.Decrypt(encBody, encLen, seq);
                if (!plaintext)
                {
                    Log::Debug("[{}] MoveRsp decrypt failed", _name);
                    return;
                }

                Proto::MoveRsp moveRsp;
                if (moveRsp.ParseFromArray(plaintext->Data(), static_cast<int>(plaintext->Size())))
                {
                    _stats.RecordMoveRcvd();
                    if (_scenario)
                    {
                        _scenario->OnMoveRsp(moveRsp.sequence());
                    }
                }
                break;
            }
            default:
                Log::Debug("[{}] unhandled msgID={}", _name, msgID);
                break;
        }
    }

    // ── 场景回调 → 网络操作 ──

    void VirtualClient::SendHeartbeat()
    {
        if (_state != ClientState::InWorld || !_socket)
        {
            return;
        }

        Proto::HeartbeatReq req;
        auto nowMs = static_cast<uint64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        req.set_client_time(nowMs);

        auto data = req.SerializeAsString();
        BuildAndSendPacket(Proto::MSG_HEARTBEAT_REQ,
                          reinterpret_cast<const uint8 *>(data.data()),
                          data.size());

        _stats.RecordHeartbeatSent();
    }

    void VirtualClient::SendMove(uint32 sequence, float x, float y, float z, float speed)
    {
        if (_state != ClientState::InWorld || !_socket)
        {
            return;
        }

        Proto::MoveReq req;
        req.set_sequence(sequence);
        req.mutable_position()->set_x(x);
        req.mutable_position()->set_y(y);
        req.mutable_position()->set_z(z);
        req.set_speed(speed);

        auto rawBody = req.SerializeAsString();

        // AES-GCM 加密
        auto encrypted = _crypto.Encrypt(
            reinterpret_cast<const uint8 *>(rawBody.data()), rawBody.size());
        if (encrypted.Size() == 0)
        {
            Log::Debug("[{}] MoveReq encrypt failed", _name);
            return;
        }

        // 构建 PacketHeader: [length:4B][msgID:4B][sessionID:4B][encrypted]
        uint32     totalLen = static_cast<uint32>(sizeof(PacketHeader) + encrypted.Size());
        ByteBuffer frame    = ByteBuffer::Own(totalLen);
        frame.WriteUint32(totalLen);
        frame.WriteUint32(Proto::MSG_MOVE_REQ);
        frame.WriteUint32(_sessionID);
        frame.WriteBytes(encrypted.Data(), encrypted.Size());

        _socket->Send(std::move(frame));
        _stats.RecordMoveSent();
    }

    // ── Tick ──

    void VirtualClient::StartTick()
    {
        _tickTimer.expires_after(std::chrono::milliseconds(20));
        _tickTimer.async_wait([self = shared_from_this()](const asio::error_code &ec) {
            self->OnTick(ec);
        });
    }

    void VirtualClient::OnTick(const asio::error_code &ec)
    {
        if (ec)
        {
            return; // timer cancelled
        }

        if (_state != ClientState::InWorld)
        {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastTick);
        _lastTick = now;

        if (_scenario && elapsed.count() > 0)
        {
            _scenario->OnTick(static_cast<uint32>(elapsed.count()));
        }

        // 循环定时器
        _tickTimer.expires_after(std::chrono::milliseconds(20));
        _tickTimer.async_wait([self = shared_from_this()](const asio::error_code &ec2) {
            self->OnTick(ec2);
        });
    }

    // ── 构建并发送 PacketHeader 帧 ──

    void VirtualClient::BuildAndSendPacket(uint32 msgID, const uint8 *body, size_t bodyLen)
    {
        uint32     totalLen = static_cast<uint32>(sizeof(PacketHeader) + bodyLen);
        ByteBuffer frame    = ByteBuffer::Own(totalLen);
        frame.WriteUint32(totalLen);
        frame.WriteUint32(msgID);
        frame.WriteUint32(_sessionID);
        frame.WriteBytes(body, bodyLen);

        if (_socket)
        {
            _socket->Send(std::move(frame));
        }
    }

    // ── 断开 ──

    void VirtualClient::Disconnect()
    {
        _state = ClientState::Disconnecting;

        if (_scenario)
        {
            _scenario->OnExit();
        }

        _tickTimer.cancel();

        if (_socket)
        {
            _socket->Close();
            _socket.reset();
        }
    }

    void VirtualClient::DoDisconnect()
    {
        Disconnect();
    }

    // ── 辅助 ──

    void VirtualClient::EnterFailedState(const std::string &reason)
    {
        _state = ClientState::Failed;
        Log::Error("[{}] FAILED: {}", _name, reason);

        if (_scenario)
        {
            _scenario->OnExit();
        }

        if (_socket)
        {
            _socket->Close();
            _socket.reset();
        }
    }

} // namespace MMO::TestClient
