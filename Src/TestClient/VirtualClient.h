/**
 * @file VirtualClient.h
 * @brief 单个虚拟客户端——回调驱动的异步状态机
 *
 * 状态转换：
 *   Idle → ConnectingLogin → SendingAuth → ConnectingGate
 *        → SendingEnterWorld → InWorld → Disconnecting
 *
 * 每个 VirtualClient 内部持有自己的 CryptoSession + 随机种子，
 * 不共享 SessionKey 或其他安全材料。
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <chrono>

#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"
#include "Common/Crypto/EcdhX25519.h"
#include "Common/Crypto/SessionToken.h"
#include "Common/Network/CryptoSession.h"
#include "Common/Network/PacketHeader.h"

namespace MMO
{
    class IOContextPool;
    class TCPSocket;
} // namespace MMO

namespace MMO::TestClient
{

    class Scenario;
    class StatsCollector;

    /**
     * @brief 虚拟客户端状态
     */
    enum class ClientState : uint8
    {
        Idle,
        ConnectingLogin,
        SendingAuth,
        ConnectingGate,
        SendingEnterWorld,
        InWorld,
        Failed,
        Disconnecting,
    };

    /**
     * @brief 单个虚拟客户端
     *
     * 线程模型：所有操作在 asio io_context 线程中执行。
     * 回调驱动的异步状态机，无需显式同步。
     */
    class VirtualClient : public std::enable_shared_from_this<VirtualClient>
    {
    public:
        /**
         * @brief 构造
         * @param clientID    客户端编号（0..N-1）
         * @param username    登录用户名
         * @param password    登录密码
         * @param loginHost   LoginServer 地址
         * @param loginPort   LoginServer 端口
         * @param stats       统计收集器
         * @param pool        IO 线程池
         * @param scenario    进入世界后的行为
         */
        VirtualClient(uint32                 clientID,
                     std::string            username,
                     std::string            password,
                     std::string            loginHost,
                     uint16                 loginPort,
                     StatsCollector        &stats,
                     IOContextPool         &pool,
                     std::unique_ptr<Scenario> scenario);

        ~VirtualClient();

        VirtualClient(const VirtualClient &)            = delete;
        VirtualClient &operator=(const VirtualClient &) = delete;

        // ── 生命周期 ──

        /// 启动：连接 LoginServer
        void Start();

        /// 断开：清理所有连接
        void Disconnect();

        /// 客户端唯一标识
        uint32 ClientID() const
        {
            return _clientID;
        }
        const std::string &Name() const
        {
            return _name;
        }

        // ── 场景回调（Scenario → 本机）──

        /// 发送心跳
        void SendHeartbeat();

        /// 发送移动请求
        void SendMove(uint32 sequence, float x, float y, float z, float speed);

        // ── 状态查询 ──

        ClientState State() const
        {
            return _state;
        }
        bool IsConnected() const
        {
            return _state == ClientState::InWorld;
        }

    private:
        // ── 状态机步骤 ──

        void OnLoginConnected(const asio::error_code &ec);
        void SendAuthReq();
        void OnAuthRsp(uint32 msgID, uint32 /*sessionID*/, const uint8 *body, size_t len);
        void ParseGateIPs(const std::string &gateIPs);
        void ConnectToGate();
        void OnGateConnected(const asio::error_code &ec);
        void SendEnterWorldReq();
        void OnEnterWorldRsp(uint32 msgID, uint32 /*sessionID*/, const uint8 *body, size_t len);
        void OnGateMessage(uint32 msgID, uint32 /*sessionID*/, const uint8 *body, size_t len);
        void OnSocketError(const asio::error_code &ec);

        // ── 辅助 ──

        void BuildAndSendPacket(uint32 msgID, const uint8 *body, size_t bodyLen);
        void LogError(const std::string &msg);
        void EnterFailedState(const std::string &reason);
        void DoDisconnect();

        uint32        _clientID;
        std::string   _name;
        std::string   _username;
        std::string   _password;
        std::string   _loginHost;
        uint16        _loginPort;
        StatsCollector      &_stats;
        IOContextPool       &_pool;

        // 状态
        ClientState  _state = ClientState::Idle;

        // 安全材料
        uint64       _clientRandom = 0;  // 客户端生成的随机数（nonce 下半部分）
        ByteBuffer   _sessionKey;        // 32B 共享密钥（ECDH → SHA-256）
        CryptoSession _crypto;

        // SessionToken（从 LoginAuthRsp）
        ByteBuffer   _sessionToken;      // 46B

        // Gate 地址（从 LoginAuthRsp 解析）
        std::string  _gateHost;
        uint16       _gatePort = 0;

        // 连接
        std::shared_ptr<TCPSocket>  _socket;
        uint32                      _sessionID = 0;

        // 行为
        std::unique_ptr<Scenario>   _scenario;
        asio::steady_timer          _tickTimer;

        void StartTick();
        void OnTick(const asio::error_code &ec);

        std::chrono::steady_clock::time_point _lastTick;
    };

} // namespace MMO::TestClient
