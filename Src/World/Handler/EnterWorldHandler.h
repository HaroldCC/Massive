#pragma once

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"
#include "Common/ECS/Scene.h"
#include "World/WorldSession.h"

#include <functional>

namespace MMO
{
    using GateSendFunc = std::function<void(uint32 sessionID, uint32 msgID, ByteBuffer body)>;

    class EnterWorldHandler
    {
    public:
        /**
         * @brief 处理 EnterWorldReq
         * @param sessionID    Gate sessionId
         * @param body         protobuf 请求体
         * @param len          长度
         * @param sessions     会话表（IO 线程读锁外，此处由 LogicThread 独占）
         * @param lss          LoginServerSecret（SessionToken 验证）
         * @param gateServerID Gate 实例 ID
         * @param defaultScene 默认场景（实体创建目标）
         * @param gateSendFn   出站回调
         */
        static void Handle(uint32                                    sessionID,
                           const uint8                              *body,
                           size_t                                    len,
                           std::unordered_map<uint32, WorldSession> &sessions,
                           const uint8                              *lss,
                           uint16                                    gateServerID,
                           ECS::Scene                               &defaultScene,
                           GateSendFunc                              gateSendFunc);
    };
} // namespace MMO