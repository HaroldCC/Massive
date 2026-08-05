/**
 * @file WorldSession.h
 * @brief WorldServer 侧玩家会话——sessionId ↔ Entity + CryptoSession + inbox
 *
 * 每个已登录玩家对应一个 WorldSession，存储于 _sessions[sessionID]。
 * Gate 断线时 WorldSession 保留，Entity 加 DisconnectedTag 等待重连。
 */
#pragma once

#include <chrono>

#include "Common/Core/Types.h"
#include "Common/Network/CryptoSession.h"
#include "Common/Queue/LogicMessage.h"
#include "Common/Queue/MPSCQueue.h"
#include "Common/ECS/EntityID.h"

namespace MMO
{

    /**
     * @brief WorldServer 侧玩家会话（设计文档 §5.3）
     *
     * 真理之源——所有玩家相关状态集中在此结构体中。
     * _sessions 是唯一映射表，IO 线程和 LogicThread 通过它协作：
     *   IO 线程：收包→查 _sessions（读锁）→ ws.inbox.Enqueue
     *   LogicThread：遍历 _sessions → ws.inbox.DrainAll → 处理消息
     */
    struct WorldSession
    {
        uint32        sessionID = 0;    // Gate 分配的 sessionID
        uint32        accountID = 0;    // 玩家账号 ID
        ECS::EntityID entityID  = 0;    // 对应 ECS EntityID（可无效，断线时为 kInvalidEntityID）
        CryptoSession crypto;           // AES-256-GCM 加解密上下文
        uint16        gateServerID = 0; // 当前连接的 Gate 实例 ID
        uint32        gateConnIdx  = 0; // 对应 Gate 连接在 GateConnectionMgr 中的索引

        // IO 线程 → LogicThread Per-Session 独立队列（无锁）
        MPSCQueue<LogicMessage> inbox;

        std::chrono::steady_clock::time_point lastRecvTime;
        bool                                  disconnected = false; // Gate 断线等待重连中
    };

} // namespace MMO
