/**
 * @file LoginServer.h
 * @brief LoginServer 主类——短 TCP 连接认证进程
 *
 * 接受 Client 连接 → 收 LoginAuthReq → 异步认证 → 回 LoginAuthRsp → 断开。
 *
 * 认证链路（6 步，3 个线程参与）：
 *   IO 线程: RateLimiter → 发起 DB 异步查询
 *   主 线 程: DB 回调 → argon2id 验密码 → ECDH → SessionToken 签发
 *   IO 线程: asio::post → Serialize → Send → Close
 */
#pragma once

#include <atomic>
#include <memory>
#include <optional>

#include <asio/post.hpp>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"
#include "Common/Crypto/EcdhX25519.h"
#include "Common/Crypto/SessionToken.h"
#include "Common/Network/IOContextPool.h"
#include "Common/Network/MessageDispatcher.h"
#include "Common/Network/TCPAcceptor.h"

#include "Login/LoginConfig.h"
#include "Login/RateLimiter.h"

#include "Login.pb.h"

namespace MMO::DB::AutoGen
{
    struct accounts_row;
} // namespace MMO::DB::AutoGen

namespace MMO
{

    class TCPSocket;

    class LoginServer
    {
    public:
        bool Init(const LoginConfig &cfg);
        void Run();
        void Stop();

    private:
        /**
         * @brief IO 线程：处理认证请求（RateLimiter → 发起 DB 异步查询）
         */
        void HandleLoginAuthReq(std::shared_ptr<TCPSocket> socket,
                                const Proto::LoginAuthReq &req,
                                const std::string         &clientIP);

        /**
         * @brief 主线程（DB 回调）：验证密码 + ECDH + SessionToken + 回包
         */
        void OnAuthDBCallback(std::shared_ptr<TCPSocket>    socket,
                              std::string                   clientIP,
                              const std::string            &username,
                              const std::string            &password,
                              const std::string            &clientDHKey,
                              std::optional<DB::AutoGen::accounts_row> row);

        /**
         * @brief IO 线程：认证成功 → 构建 LoginAuthRsp + Send + Close
         */
        void SendAuthSuccess(std::shared_ptr<TCPSocket>  socket,
                             const Crypto::EcdhKeyPair  &keyPair,
                             const Crypto::SessionToken &token);

        /**
         * @brief IO 线程：认证失败 → 构建错误响应 + Send + Close
         */
        void SendAuthFailure(std::shared_ptr<TCPSocket> socket, uint32 errorCode);

        static constexpr uint32 kTokenExpireSec = 7200; // SessionToken 过期时间（2 小时）

        std::unique_ptr<IOContextPool>                _ioPool;
        std::unique_ptr<TCPAcceptor>                  _acceptor;
        MessageDispatcher<std::shared_ptr<TCPSocket>> _dispatcher;
        RateLimiter                                   _rateLimiter;
        uint8                                         _lss[LoginConfig::kLSSSize] = {}; // 32B LSS
        uint16                                        _worldServerID              = 1;
        std::vector<std::string>                      _gateIPs;
        std::atomic<bool>                             _running {false};
    };

} // namespace MMO
