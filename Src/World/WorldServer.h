/**
 * @file WorldServer.h
 * @brief WorldServer 主类——游戏逻辑核心进程
 *
 * 线程模型：
 *   IOContextPool (N 线程): Gate/Center 网络 IO
 *   LogicThread (1 线程):   Per-Session inbox Drain + 20ms Tick 游戏逻辑
 *   DBWorkerPool (3-5 线程): libpq 阻塞查询
 */
#pragma once

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "Common/Log/Log.h"
#include "Common/Network/IOContextPool.h"
#include "Common/Network/MessageDispatcher.h"
#include "Common/Network/PacketHeader.h"
#include "Common/Network/TCPAcceptor.h"

#include "World/CenterClient.h"
#include "World/GateConnection.h"
#include "World/SceneManager.h"
#include "World/Handler/EnterWorldHandler.h"
#include "World/Handler/MoveHandler.h"
#include "World/LogicThread.h"
#include "World/WorldConfig.h"
#include "World/WorldSession.h"

namespace MMO
{

    class WorldServer
    {
    public:
        bool Init(const WorldConfig &cfg);
        void Run();
        void Stop();

    private:
        // ── Init 阶段 ──
        bool InitCenterClient(const WorldConfig &cfg);
        bool InitGateAcceptor(const WorldConfig &cfg);

        // ── 消息分发注册 ──
        void RegisterHandlers();

        // ── LogicThread 回调 ──
        void OnTick(std::chrono::milliseconds elapsed);
        void OnMessage(uint32 sessionID, WorldSession &ws, const LogicMessage &msg);
        void OnPreProcess();
        void OnPostFlush();

        // ── 未路由消息处理（EnterWorldReq Fallback）──
        void ProcessUnroutedMessages();

        /**
         * @brief 加密 protobuf 消息并发送到客户端
         * @tparam TMsg protobuf 消息类型
         * @param sessionID  目标 Session
         * @param msgID      消息 ID（EMsgID）
         * @param msg        消息
         */
        template <typename TMsg>
        void SendToClient(uint32 sessionID, uint32 msgID, const TMsg &msg)
        {
            auto data = msg.SerializeAsString();
            auto buf  = ByteBuffer::Copy(
                reinterpret_cast<const uint8 *>(data.data()), data.size());

            auto it = _sessions.find(sessionID);
            if (it == _sessions.end())
            {
                Log::Warn("SendToClient: session {} not found", sessionID);
                return;
            }

            // 加密 = [Seq:4B][Ciphertext+Tag]
            auto encrypted = it->second.crypto.Encrypt(buf.Data(), buf.Size());
            if (encrypted.Size() == 0)
            {
                return;
            }

            // 构建完整包: [PacketHeader:12B][encrypted]
            uint32 totalLen = static_cast<uint32>(sizeof(PacketHeader) + encrypted.Size());
            auto   frame    = ByteBuffer::Own(totalLen + sizeof(uint32)); // +4 for LengthPrefix
            frame.WriteUint32(totalLen);
            frame.WriteUint32(msgID);
            frame.WriteUint32(sessionID);
            frame.WriteBytes(encrypted.Data(), encrypted.Size());

            _gateConnMgr->SendToGate(it->second.gateServerID, sessionID, std::move(frame));
        }

        // ── 消息分发（按 msgID 查表）──
        MessageDispatcher<uint32> _dispatcher;  // context = sessionID

        // ── 组件 ──
        std::unique_ptr<IOContextPool>   _ioPool;
        std::unique_ptr<TCPAcceptor>     _gateAcceptor;
        std::unique_ptr<CenterClient>    _centerClient;
        std::unique_ptr<GateConnectionMgr> _gateConnMgr;
        LogicThread                      _logicThread;

        // ── Session 存储（IO 线程读锁 + LogicThread 独占写）──
        std::shared_mutex                        _sessionsMtx;
        std::unordered_map<uint32, WorldSession> _sessions;

        // ── 场景 ──
        SceneManager _sceneMgr;

        // ── 配置 ──
        WorldConfig _config;
        std::atomic<bool> _running {false};
    };

} // namespace MMO
