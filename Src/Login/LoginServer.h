/**
 * @file LoginServer.h
 * @brief LoginServer 主类——短 TCP 连接认证进程
 *
 * 接受 Client 连接 → 收 LoginAuthReq → 同步认证 → 回 LoginAuthRsp → 断开。
 * 认证链路：RateLimiter → DB 查 accounts → argon2id 验密码 → ECDH → SessionToken 签发。
 */
#pragma once

#include <memory>

#include "Common/Core/Types.h"
#include "Common/Network/IOContextPool.h"
#include "Common/Network/MessageDispatcher.h"
#include "Common/Network/TCPAcceptor.h"

#include "Login/LoginConfig.h"
#include "Login/RateLimiter.h"

namespace MMO
{

class TCPSocket;

class LoginServer
{
public:
    bool Init(const LoginConfig& cfg);
    void Run();
    void Stop();

private:
    void OnNewConnection(std::shared_ptr<TCPSocket> socket);
    void HandleLoginAuthReq(std::shared_ptr<TCPSocket> socket,
                            const uint8* body, size_t len,
                            const std::string& clientIP);

    std::unique_ptr<IOContextPool> _ioPool;
    std::unique_ptr<TCPAcceptor>   _acceptor;
    MessageDispatcher<std::shared_ptr<TCPSocket>> _dispatcher;
    RateLimiter                    _rateLimiter;
    LoginConfig                    _config;
    bool                           _running = false;
};

} // namespace MMO
