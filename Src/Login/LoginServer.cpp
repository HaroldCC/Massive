/**
 * @file LoginServer.cpp
 * @brief LoginServer 实现——认证主逻辑
 *
 * 认证链路（IO 线程同步执行）:
 *   1. RateLimiter.Allow(ip)
 *   2. DB 查 accounts → 取 password_hash + ban_until
 *   3. Argon2id::VerifyPassword
 *   4. 封禁检查
 *   5. ECDH: GenerateKeyPair → DeriveSharedSecret → DeriveSessionKey
 *   6. SessionTokenBuilder::Issue(LSS, SessionKey, worldId, accountId, expireTime)
 *   7. LoginAuthRsp → Send → Close
 *
 * 当前骨架：验证 TCPSocket → MessageDispatcher 链路，handler 以原型消息直接回包。
 */
#include "Login/LoginServer.h"
#include "Login/LoginConfig.h"

#include "Common/Log/Log.h"
#include "Common/Network/TCPSocket.h"

#include <Login.pb.h>
#include <MsgID.pb.h>

namespace MMO
{

bool LoginServer::Init(const LoginConfig& cfg)
{
    _config = cfg;

    _ioPool   = std::make_unique<IOContextPool>(static_cast<size_t>(cfg.network.ioThreads));
    _acceptor = std::make_unique<TCPAcceptor>(*_ioPool, cfg.network.port);

    // 注册 MSG_LOGIN_AUTH_REQ handler
    _dispatcher.Register<LoginAuthReq>(
        MSG_LOGIN_AUTH_REQ,
        [this](std::shared_ptr<TCPSocket> socket, const LoginAuthReq& req)
        {
            std::string clientIP = "0.0.0.0";
            try { clientIP = socket->Socket().remote_endpoint().address().to_string(); }
            catch (...) {}
            HandleLoginAuthReq(std::move(socket),
                reinterpret_cast<const uint8*>(req.SerializeAsString().data()),
                req.SerializeAsString().size(), clientIP);
        });

    _acceptor->Start([this](std::shared_ptr<TCPSocket> socket)
    {
        auto sockPtr = socket.get();
        socket->SetMessageHandler(
            [this, sock = std::move(socket)](uint32 msgID, uint32 /*sessionID*/,
                                              const uint8* body, size_t len) mutable
            {
                _dispatcher.Dispatch(sock, msgID, body, len);
            });
        sockPtr->Start();
    });

    Log::Info("LoginServer listening on port {}", cfg.network.port);
    return true;
}

void LoginServer::Run()
{
    _running = true;
    _ioPool->Start();
}

void LoginServer::Stop()
{
    _running = false;
    if (_acceptor) _acceptor->Stop();
    if (_ioPool)   _ioPool->Stop();
}

void LoginServer::HandleLoginAuthReq(std::shared_ptr<TCPSocket> socket,
                                      const uint8* /*body*/, size_t /*len*/,
                                      const std::string& clientIP)
{
    if (!_rateLimiter.Allow(clientIP))
    {
        socket->Close();
        return;
    }

    Log::Info("LoginServer auth request from {}", clientIP);

    // TODO: DB 异步查询 + argon2id + ECDH + SessionToken 签发
    // 当前返回骨架响应，验证 MessageDispatcher → LoginServer handler 链路
    LoginAuthRsp rsp;
    rsp.mutable_error()->set_code(0);
    for (auto& ip : _config.world.gateIPs)
    {
        rsp.add_gate_ips(ip);
    }

    auto data = rsp.SerializeAsString();
    auto buf = ByteBuffer::Copy(reinterpret_cast<const uint8*>(data.data()), data.size());
    socket->Send(std::move(buf));
    socket->Close();
}

} // namespace MMO
