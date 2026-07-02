/**
 * @file GateConnection.h
 * @brief WorldServer 侧 Gate 连接管理
 *
 * 管理到各 GateServer 的内部 TCP 连接（WorldServer 作为 acceptor）。
 * IO 线程收包 → 读 InternalHeader → 查 _sessions（读锁）→ ws.inbox.Enqueue
 *
 * Gate 配置信息包含：
 *   - gateServerID / 地址 / 连接状态
 *
 * Outbound: WorldSession::Send → GateConnectionMgr::SendToGate → GateConn.TCPSocket.Send
 */
#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "Common/Core/Types.h"
#include "Common/Network/TCPSocket.h"

#include "World/WorldSession.h"

namespace MMO
{

    class GateConnectionMgr
    {
    public:
        /**
         * @brief 接受 Gate 内部 TCP 连接
         * @param gateID   Gate 实例 ID
         * @param socket   已连接的 TCPSocket
         */
        void AcceptConnection(uint16 gateID, std::shared_ptr<TCPSocket> socket);

        /**
         * @brief 发送业务消息到指定 Gate
         * @param gateID      目标 Gate 实例 ID
         * @param sessionID   InternalHeader.sessionID
         * @param payload     已加密的 body（不含 InternalHeader）
         */
        void SendToGate(uint16 gateID, uint32 sessionID, ByteBuffer payload);

        /**
         * @brief IO 线程入口：从 Gate 连接收包，解密后 Enqueue 到对应 Session inbox
         * @param sessionID   InternalHeader 解析出的 sessionID
         * @param data        完整帧（已剥离 LengthPrefix）
         * @param len         数据长度
         */
        void OnGateMessage(uint32 sessionID, const uint8 *data, size_t len);

        /**
         * @brief 处理 Gate 控制消息（IO 线程）
         * 拆 `[InternalMsgID:4B][body]` 后 Enqueue 到 _ctrlQueue，由 LogicThread 消费。
         */
        void HandleControlMessage(uint32 ctrlMsgID, const uint8 *data, size_t len);

        // 注册/注销 IO 线程可访问的 _sessions 指针
        void SetSessionsPtr(std::shared_mutex *mtx, std::unordered_map<uint32, WorldSession> *sessions)
        {
            _sessionsMtx = mtx;
            _sessions    = sessions;
        }

        // Fallback / 控制消息队列（IO 线程写，LogicThread 在 OnTick 中 DrainAll）
        MPSCQueue<LogicMessage> &GetUnroutedQueue()
        {
            return _unroutedQueue;
        }

        MPSCQueue<LogicMessage> &GetCtrlQueue()
        {
            return _ctrlQueue;
        }

    private:
        struct GateConnection
        {
            uint16                     gateID = 0;
            std::shared_ptr<TCPSocket> socket;
        };

        std::unordered_map<uint16, std::unique_ptr<GateConnection>> _gateConns;
        std::mutex                                                  _gateMutex;

        // 指向 WorldServer::_sessions 的指针（IO 线程读锁访问）
        std::shared_mutex                        *_sessionsMtx = nullptr;
        std::unordered_map<uint32, WorldSession> *_sessions    = nullptr;

        // Fallback / 控制消息队列（IO 线程写，LogicThread 读）
        MPSCQueue<LogicMessage> _unroutedQueue;
        MPSCQueue<LogicMessage> _ctrlQueue;
    };

} // namespace MMO
