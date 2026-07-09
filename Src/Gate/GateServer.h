/**
 * @file GateServer.h
 * @brief GateServer 主类——无状态连接代理
 *
 * 纯 IO 进程，无 LogicThread，无 DB 访问。
 * 职责：
 *   1. 接受客户端 TCP 连接（PacketHeader 帧协议）
 *   2. 解析 SessionToken[0..1] → 路由到对应 WorldServer
 *   3. Client↔World 消息透传（Body 零解析）
 *   4. 心跳自回 + 超时断开
 *   5. 连接级限流（单 IP + 全局上限）
 */
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

#include "Common/Core/Types.h"
#include "Common/Network/IOContextPool.h"
#include "Common/Network/MessageDispatcher.h"
#include "Common/Network/TCPAcceptor.h"
#include "Common/Network/TCPSocket.h"

#include "Gate/GateConfig.h"
#include "Gate/GateSession.h"

// Proto 消息类型前向声明（完整定义在 .pb.h 中，由 .cpp include）
namespace MMO::Proto
{
    class LoginEnterWorldReq;
    class HeartbeatReq;
} // namespace MMO::Proto

namespace MMO
{

    /**
     * @brief GateServer 主类
     */
    class GateServer
    {
    public:
        /**
         * @brief 初始化 GateServer
         * @param cfg  GateConfig
         * @return 成功返回 true
         */
        bool Init(const GateConfig &cfg);

        /**
         * @brief 启动 IO 线程池 + 超时检查
         */
        void Run();

        /**
         * @brief 停止所有组件
         */
        void Stop();

    private:
        // ── TCPAcceptor 回调 ──

        /**
         * @brief 新客户端连接
         * @param socket  已 accept 的 TCPSocket
         */
        void OnNewClientConnection(std::shared_ptr<TCPSocket> socket);

        // ── 消息分发 ──

        /**
         * @brief 客户端消息入口
         * @param session  来源 GateSession
         * @param msgID    消息 ID
         * @param body     消息体
         * @param len      消息体长度
         */
        void
        OnClientMessage(std::shared_ptr<GateSession> session, uint32 msgID, const uint8 *body, size_t len);

        /**
         * @brief 进入世界请求处理
         * @param session  客户端会话
         * @param req      反序列化后的请求
         * @param rawBody  原始 body 指针（转发用）
         * @param rawLen   原始 body 长度
         */
        void OnEnterWorldReq(std::shared_ptr<GateSession>     session,
                             const Proto::LoginEnterWorldReq &req,
                             const uint8                     *rawBody,
                             size_t                           rawLen);

        /**
         * @brief 进入世界失败回应，关闭连接
         * @param session    客户端会话
         * @param errorCode  错误码
         */
        void SendEnterWorldError(std::shared_ptr<GateSession> session, uint32 errorCode);

        /**
         * @brief 心跳请求（Gate 自回）
         * @param session  客户端会话
         * @param req      心跳请求
         */
        void OnHeartbeatReq(std::shared_ptr<GateSession> session, const Proto::HeartbeatReq &req);

        // ── World 连接管理 ──

        /**
         * @brief 连接到配置中所有 WorldServer
         * @param cfg  GateConfig
         */
        void ConnectToWorlds(const GateConfig &cfg);

        /**
         * @brief 连接到单个 WorldServer
         * @param addr  "ip:port" 格式地址
         */
        void ConnectToWorld(const std::string &addr);

        /**
         * @brief 退避重连
         * @param addr  "ip:port" 格式地址
         */
        void ScheduleReconnect(const std::string &addr);

        /**
         * @brief World 连接建立成功回调
         * @param addr     WorldServer 地址
         * @param socket   已连接 TCPSocket
         */
        void OnWorldConnected(const std::string &addr, std::shared_ptr<TCPSocket> socket);

        /**
         * @brief World 连接断开回调
         * @param addr  WorldServer 地址
         */
        void OnWorldDisconnected(const std::string &addr);

        /**
         * @brief World → Gate 消息（LengthPrefix 拆包后）
         * @param worldAddr  WorldServer 地址
         * @param data       接收数据（不含 LengthPrefix）
         * @param len        数据长度
         */
        void OnWorldMessage(const std::string &worldAddr, const uint8 *data, size_t len);

        /**
         * @brief 转发消息到 World
         * @param session  客户端会话
         * @param msgID    消息 ID
         * @param body     消息体
         * @param bodyLen  消息体长度
         */
        void ForwardToWorld(const std::shared_ptr<GateSession> &session,
                            uint32                              msgID,
                            const uint8                        *body,
                            size_t                              bodyLen);

        // ── 会话管理 ──

        /**
         * @brief 原子分配 sessionId
         */
        uint32 AllocateSessionID()
        {
            return _nextSessionID.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief session 断开处理，清理路由和映射
         * @param sessionID  断开的 sessionId
         */
        void OnSessionDisconnect(uint32 sessionID);

        /**
         * @brief 清理 session
         * @param sessionID  目标 sessionId
         */
        void RemoveSession(uint32 sessionID);

        // ── 限流 ──

        /**
         * @brief 是否允许新连接
         * @param clientIP  客户端 IP
         * @return 允许返回 true
         */
        bool AllowNewConnection(const std::string &clientIP);

        /**
         * @brief 连接关闭时更新计数器
         * @param clientIP  客户端 IP
         */
        void OnConnectionClosed(const std::string &clientIP);

        // ── 定时器 ──

        /// 检查客户端心跳超时，超时则断开
        void CheckClientTimeouts();

        /// 启动 10s 间隔周期超时检查
        void StartTimeoutCheck();

        // ── 世界路由辅助 ──

        /**
         * @brief 根据 sessionId 找到目标 World 地址
         * @param sessionID  会话 ID
         * @return World 地址，未路由返回空字符串
         */
        std::string GetWorldRoute(uint32 sessionID) const;

        /**
         * @brief 找到第一个可用的 World
         * @return World 地址，无可用返回空字符串
         */
        std::string PickWorldServer() const;

        // ── 组件 ──

        std::unique_ptr<IOContextPool> _ioPool;
        std::unique_ptr<TCPAcceptor>   _acceptor;

        // 连接管理
        std::unordered_map<uint32, std::shared_ptr<GateSession>> _sessions;

        // session 路由表：sessionID → (worldAddr, clientSession)
        struct SessionRoute
        {
            std::string                worldAddr;
            std::weak_ptr<GateSession> clientSession;
        };

        std::unordered_map<uint32, SessionRoute> _sessionRoutes;

        // Gate↔World 连接
        struct WorldConnection
        {
            std::shared_ptr<TCPSocket> socket;
            bool                       connected        = false;
            uint32                     reconnectDelayMs = 1000;
        };

        std::unordered_map<std::string, std::unique_ptr<WorldConnection>> _worldConns;

        // World 连接上的 session 列表（用于断线时清理）
        std::unordered_map<std::string, std::vector<uint32>> _worldSessionMap;

        /// 保护 _sessions, _sessionRoutes, _worldConns, _worldSessionMap 的粗粒度锁
        mutable std::mutex _gateMutex;

        // 限流
        struct IPEntry
        {
            uint32 connCount;
        };

        std::unordered_map<std::string, IPEntry> _ipConnections;
        std::mutex                               _ipMutex;
        uint32                                   _totalConnections = 0;

        std::unique_ptr<asio::steady_timer> _timeoutTimer;

        std::atomic<uint32> _nextSessionID {1};
        GateConfig          _config;
        std::atomic<bool>   _running {false};
    };

} // namespace MMO
