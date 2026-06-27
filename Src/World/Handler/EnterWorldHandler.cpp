/**
 * @file EnterWorldHandler.cpp
 * @brief EnterWorldHandler 实现
 */

#include "World/Handler/EnterWorldHandler.h"

#include "Common/Crypto/SessionToken.h"
#include "Common/ECS/Scene.h"
#include "Common/Log/Log.h"

#include <Login.pb.h>
#include <MsgID.pb.h>

#include <chrono>
#include <ctime>

namespace MMO
{

    void EnterWorldHandler::Handle(
        uint32                                    sessionID,
        const uint8                              *body,
        size_t                                    len,
        std::unordered_map<uint32, WorldSession> &sessions,
        const uint8                              *lss,
        uint16                                    gateServerID,
        ECS::Scene                               &defaultScene,
        std::function<void(uint32, ByteBuffer)>   gateSendFn)
    {
        Proto::LoginEnterWorldReq req;
        if (!req.ParseFromArray(body, static_cast<int>(len)))
        {
            Log::Warn("EnterWorld: protobuf parse failed ({} bytes)", len);
            SendError(2001, "Invalid request", gateSendFn);
            return;
        }

        // 解析 SessionToken
        if (static_cast<size_t>(req.session_token().size()) != Crypto::SessionToken::kTotalSize)
        {
            Log::Warn("EnterWorld: invalid session token size={}", req.session_token().size());
            SendError(2002, "Invalid session token", gateSendFn);
            return;
        }

        auto tokenOpt = Crypto::SessionToken::FromBuffer(
            reinterpret_cast<const uint8 *>(req.session_token().data()),
            static_cast<size_t>(req.session_token().size()));
        if (!tokenOpt)
        {
            SendError(2002, "Invalid session token", gateSendFn);
            return;
        }

        auto payloadOpt = Crypto::SessionTokenBuilder::Verify(lss, *tokenOpt);
        if (!payloadOpt)
        {
            Log::Debug("EnterWorld: token verify failed (expired or tampered)");
            SendError(2003, "Token expired or invalid", gateSendFn);
            return;
        }

        uint32  accountID    = payloadOpt->accountId;
        uint8  *sessionKey   = payloadOpt->sessionKey.Data();
        auto    clientRandom = req.nonce();

        // 检查是否重连
        for (auto &[sid, ws] : sessions)
        {
            if (ws.accountID == accountID)
            {
                Log::Info("EnterWorld: reconnect detected for accountID={}", accountID);
                HandleReconnect(sessionID, gateServerID, sessions, accountID,
                                reinterpret_cast<const uint8 *>(req.reconnect_seed().data()),
                                static_cast<size_t>(req.reconnect_seed().size()),
                                clientRandom, gateSendFn);
                return;
            }
        }

        // 首次登录
        HandleFirstLogin(sessionID, gateServerID, sessions, accountID,
                         sessionKey, clientRandom, defaultScene, gateSendFn);
    }

    void EnterWorldHandler::HandleFirstLogin(
        uint32                                    sessionID,
        uint16                                    gateServerID,
        std::unordered_map<uint32, WorldSession> &sessions,
        uint32                                    accountID,
        const uint8                              *sessionKey,
        uint64                                    clientRandom,
        ECS::Scene                               &defaultScene,
        std::function<void(uint32, ByteBuffer)>   gateSendFn)
    {
        auto entity = defaultScene.CreateEntity();

        CryptoSession crypto;
        crypto.Init(sessionKey, clientRandom);

        WorldSession ws;
        ws.sessionID    = sessionID;
        ws.accountID    = accountID;
        ws.entity       = entity;
        ws.crypto       = std::move(crypto);
        ws.gateServerID = gateServerID;
        ws.lastRecvTime = std::chrono::steady_clock::now();
        ws.disconnected = false;

        sessions[sessionID] = std::move(ws);

        Log::Info("EnterWorld: accountID={} → entity=({},{}) sessionID={}",
                  accountID, entity.sceneId, entity.entityId, sessionID);

        SendRsp(entity.entityId, entity.sceneId, 0.0f, 0.0f, 0.0f, gateSendFn);
    }

    void EnterWorldHandler::HandleReconnect(
        uint32                                    sessionID,
        uint16                                    gateServerID,
        std::unordered_map<uint32, WorldSession> &sessions,
        uint32                                    accountID,
        const uint8                              *reconnectSeed,
        size_t                                    seedLen,
        uint64                                    clientRandom,
        std::function<void(uint32, ByteBuffer)>   gateSendFn)
    {
        // 找到旧 Session
        uint32 oldSessionID = 0;
        for (auto &[sid, ws] : sessions)
        {
            if (ws.accountID == accountID)
            {
                oldSessionID = sid;
                break;
            }
        }

        auto oldEntry = sessions.extract(oldSessionID);
        auto &ws = oldEntry.mapped();
        ws.disconnected  = false;
        ws.gateServerID  = gateServerID;
        ws.lastRecvTime  = std::chrono::steady_clock::now();
        ws.sessionID     = sessionID;

        // 旋转密钥
        if (seedLen > 0)
        {
            ws.crypto.RotateReconnectKey(reconnectSeed, seedLen);
        }
        else
        {
            // 没有 reconnectSeed 时用 nonce 作为种子
            uint8 fallbackSeed[8];
            std::memcpy(fallbackSeed, &clientRandom, 8);
            ws.crypto.RotateReconnectKey(fallbackSeed, 8);
        }

        sessions[sessionID] = std::move(ws);

        Log::Info("EnterWorld: RECONNECT accountID={} sessionID {}→{}",
                  accountID, oldSessionID, sessionID);

        SendRsp(ws.entity.entityId, ws.entity.sceneId, 0.0f, 0.0f, 0.0f, gateSendFn);
    }

    void EnterWorldHandler::SendError(uint32 errorCode, const char *msg,
                                      std::function<void(uint32, ByteBuffer)> gateSendFn)
    {
        Proto::LoginEnterWorldRsp rsp;
        rsp.mutable_error()->set_code(errorCode);
        rsp.mutable_error()->set_message(msg);
        auto data = rsp.SerializeAsString();
        auto buf  = ByteBuffer::Copy(
            reinterpret_cast<const uint8 *>(data.data()), data.size());
        gateSendFn(0, std::move(buf));
    }

    void EnterWorldHandler::SendRsp(uint32 playerID, uint32 sceneID, float x, float y, float z,
                                    std::function<void(uint32, ByteBuffer)> gateSendFn)
    {
        Proto::LoginEnterWorldRsp rsp;
        rsp.mutable_error()->set_code(0);
        rsp.set_player_id(playerID);
        rsp.set_scene_id(sceneID);
        rsp.mutable_position()->set_x(x);
        rsp.mutable_position()->set_y(y);
        rsp.mutable_position()->set_z(z);

        auto data = rsp.SerializeAsString();
        auto buf  = ByteBuffer::Copy(
            reinterpret_cast<const uint8 *>(data.data()), data.size());
        gateSendFn(0, std::move(buf));
    }

} // namespace MMO
