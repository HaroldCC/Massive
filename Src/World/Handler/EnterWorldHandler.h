/**
 * @file EnterWorldHandler.h
 * @brief MSG_LOGIN_ENTER_WORLD_REQ 处理 — SessionToken 验证 → Entity 创建 → WorldSession 绑定
 */
#pragma once

#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

#include "Common/Core/Types.h"
#include "Common/ECS/Entity.h"
#include "Common/ECS/Scene.h"
#include "Common/Core/ByteBuffer.h"

#include "World/WorldSession.h"

namespace MMO
{

    class EnterWorldHandler
    {
    public:
        /**
         * @brief 处理 EnterWorld 消息
         * @param sessionID    Gate 分配的 sessionId
         * @param body         protobuf 序列化数据
         * @param len          数据长度
         * @param sessions     WorldServer::_sessions（LogicThread 独占）
         * @param lss          LoginServerSecret (32B)
         * @param gateServerID 当前 Gate 实例 ID
         * @param defaultScene 默认场景
         * @param gateSendFn   WorldSession → Gate 出站回调
         */
        /**
         * @brief 出站回调类型: (sessionID, msgID, rawBody) → 发送到客户端
         * WorldServer 侧会在回调内完成加密 + PacketHeader 包装
         */
        using GateSendFn = std::function<void(uint32, uint32, ByteBuffer)>;

        static void Handle(
            uint32                                    sessionID,
            const uint8                              *body,
            size_t                                    len,
            std::unordered_map<uint32, WorldSession> &sessions,
            const uint8                              *lss,
            uint16                                    gateServerID,
            ECS::Scene                               &defaultScene,
            GateSendFn                                gateSendFn);

    private:
        static void HandleFirstLogin(
            uint32                                    sessionID,
            uint16                                    gateServerID,
            std::unordered_map<uint32, WorldSession> &sessions,
            uint32                                    accountID,
            const uint8                              *sessionKey,
            uint64                                    clientRandom,
            ECS::Scene                               &defaultScene,
            GateSendFn                                gateSendFn);

        static void HandleReconnect(
            uint32                                    sessionID,
            uint16                                    gateServerID,
            std::unordered_map<uint32, WorldSession> &sessions,
            uint32                                    accountID,
            const uint8                              *reconnectSeed,
            size_t                                    seedLen,
            uint64                                    clientRandom,
            GateSendFn                                gateSendFn);

        static void SendError(uint32 sessionID, uint32 errorCode, const char *msg,
                              GateSendFn gateSendFn);

        static void SendRsp(uint32 sessionID, uint32 playerID, uint32 sceneID, float x, float y, float z,
                            GateSendFn gateSendFn);
    };

} // namespace MMO
