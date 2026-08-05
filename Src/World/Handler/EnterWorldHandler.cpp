#include "EnterWorldHandler.h"
#include "Common/Crypto/SessionToken.h"
#include "Common/Log/Log.h"

#include "Login.pb.h"
#include "MsgID.pb.h"
#include "World/Component/Health.h"
#include "World/Component/PlayerConnection.h"
#include "World/Component/Position.h"
#include "World/Component/Tags.h"
#include "World/WorldSession.h"

namespace MMO
{
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
    void EnterWorldHandler::Handle(uint32                                    sessionID,
                                   const uint8                              *body,
                                   size_t                                    len,
                                   std::unordered_map<uint32, WorldSession> &sessions,
                                   const uint8                              *lss,
                                   uint16                                    gateServerID,
                                   ECS::Scene                               &defaultScene,
                                   GateSendFunc                              gateSendFunc)
    {
        Proto::LoginEnterWorldReq req;
        if (!req.ParseFromArray(body, static_cast<int>(len)))
        {
            Log::Warn("EnterWorldHandler: failed to parse EnterWorldReq for session {}", sessionID);
            return;
        }

        if (static_cast<size_t>(req.session_token().size()) != Crypto::SessionToken::kTotalSize)
        {
            Log::Warn("EnterWorldHandler: invalid session token size for session {}", sessionID);
            return;
        }

        auto tokenOpt =
            Crypto::SessionToken::FromBuffer(reinterpret_cast<const uint8 *>(req.session_token().data()),
                                             req.session_token().size());
        if (!tokenOpt)
        {
            return;
        }

        auto payloadOpt = Crypto::SessionTokenBuilder::Verify(lss, *tokenOpt);
        if (!payloadOpt)
        {
            Log::Warn("EnterWorldHandler: session token verification failed for session {}", sessionID);
            return;
        }

        const uint32 accountID    = payloadOpt->accountID;
        const uint64 clientRandom = req.nonce();

        // 创建实体
        const ECS::EntityID entityID = defaultScene.CreateEntity();

        // 玩家实体组件：连接归属 + 出生位置 + 玩家标记
        //  （AOI/复制/断线处理都依赖这些——没有它们玩家不可见）
        defaultScene.EmplaceComponent<PlayerConnection>(
            entityID,
            PlayerConnection {.sessionID = sessionID, .gateServerID = gateServerID});
        defaultScene.EmplaceComponent<Position>(entityID, Position {0.0f, 0.0f, 0.0f});
        defaultScene.EmplaceComponent<Health>(entityID, Health {100, 100});
        defaultScene.GetEnttRegistry().emplace<PlayerTag>(defaultScene.ResolveEntity(entityID));

        // 玩家进格子（AOI 空间索引）
        defaultScene.GetGrid().Insert(ECS::EntityIndex(static_cast<uint32>(entityID)), 0.0f, 0.0f);

        CryptoSession crypto;
        crypto.Init(payloadOpt->sessionKey.Data(), clientRandom);

        WorldSession session {
            .sessionID    = sessionID,
            .accountID    = accountID,
            .entityID     = entityID,
            .crypto       = std::move(crypto),
            .gateServerID = gateServerID,
            .lastRecvTime = std::chrono::steady_clock::now(),
            .disconnected = false,
        };

        sessions[sessionID] = std::move(session);

        Log::Info("EnterWorldHandler: session {} account {} entered world, entityID={}",
                  sessionID,
                  accountID,
                  entityID);

        // 回包（player_id 已按决策 2 改为 uint64——直接传完整 EntityID）
        Proto::LoginEnterWorldRsp rsp;
        rsp.mutable_error()->set_code(0);
        rsp.set_player_id(static_cast<uint64>(entityID));
        rsp.set_scene_id(defaultScene.SceneID());
        rsp.mutable_position()->set_x(0.0f);
        rsp.mutable_position()->set_y(0.0f);
        rsp.mutable_position()->set_z(0.0f);

        size_t                bodySize = static_cast<size_t>(rsp.ByteSizeLong());
        auto                  buf      = ByteBuffer::Own(bodySize);
        [[maybe_unused]] bool ret      = rsp.SerializeToArray(buf.WritePtr(), static_cast<int>(bodySize));
        buf.SetWritePos(bodySize);

        gateSendFunc(sessionID, Proto::MSG_LOGIN_ENTER_WORLD_RSP, std::move(buf));
    }
} // namespace MMO