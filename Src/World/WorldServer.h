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

#include "Common/Core/Types.h"
#include "Common/Log/Log.h"
#include "Common/Network/IOContextPool.h"
#include "Common/Network/MessageDispatcher.h"
#include "Common/Network/TCPAcceptor.h"
#include "World.h"
#include "World/CenterClient.h"
#include "World/GateConnection.h"
#include "World/LogicThread.h"
#include "World/WorldConfig.h"
#include "World/WorldSession.h"
#include "ScriptEngine/DasEngine.h"
#include "ScriptEngine/GameEventBus.h"
#include "ScriptEngine/IDasHost.h"
#include "ScriptEngine/IDasModuleProvider.h"
#include "World/System/SystemReplicate.h"

#include <daScript/ast/ast.h>
#include <daScript/simulate/simulate.h>

#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace MMO
{

    class WorldServer : public IDasLangHost
    {
    public:
        bool Init(const WorldConfig &cfg);
        void Run();
        void Stop();

        void          SendRawToClient(uint32 sessionID, uint32 msgID, const uint8 *data, size_t len) override;
        GameEventBus *GetGameEventBus() override;
        ECS::Scene   *GetDefaultScene() override;
        ECS::Scene   *GetSceneByEntityID(uint64 entityID) override;

    private:
        // ── Init 阶段 ──
        bool InitCenterClient(const WorldConfig &cfg);
        bool InitGateAcceptor(const WorldConfig &cfg);
        void InitWorlds();

        // ── 消息分发注册 ──
        void RegisterHandlers();

        // ── LogicThread 回调 ──
        void OnTick(float dt);
        void OnMessage(uint32 sessionID, WorldSession &ws, const LogicMessage &msg);
        void OnPreProcess();
        void OnPostFlush();

        // ── Gate 控制消息处理 ──
        void OnControlMessage(uint32 ctrlMsgID, const uint8 *data, size_t len);
        void OnDisconnectNtf(uint32 sessionID);
        void OnSessionRebindReq(const uint8 *data, size_t len);

        // ── 断线超时 ──
        void OnDisconnectTimeout(uint32 accountID);

        // ── Center 通知 ──
        void NotifyCenterPlayerOnline(uint32 accountID);
        void NotifyCenterPlayerOffline(uint32 accountID);

        // ── 过载保护 ──
        enum class ELoadLevel : uint8
        {
            NORMAL,
            WARNING,
            DEGRADED
        };
        void       UpdateLoadLevel(size_t sessionCount, size_t pendingMessages);
        void       ApplyLoadLevel(ELoadLevel oldLevel, ELoadLevel newLevel);
        ELoadLevel _loadLevel = ELoadLevel::NORMAL;

        // ── 未路由消息处理（EnterWorldReq Fallback）──
        void ProcessUnroutedMessages();

        // ── 控制消息处理（_ctrlQueue 消费）──
        void ProcessControlMessages();

        // ── 脚本引擎 ──

        /**
         * @brief 初始化 DasLang 脚本引擎
         *
         * 编译 Script/World/main.das（含消息分发注册）
         * @return 成功返回 true
         */
        bool InitScriptEngine();

        /**
         * @brief 加密已序列化字节并发送到客户端（复制系统用）
         * @warning 必须在 LogicThread 中调用（独占 _sessions 读写权限）
         * @param sessionID 目标 Session
         * @param msgID     消息 ID（EMsgID）
         * @param data      已序列化的消息字节（protobuf body）
         * @param len       字节数
         */
        void SendEncrypted(uint32 sessionID, uint32 msgID, const uint8 *data, size_t len);

        /**
         * @brief 把事件总线内容派发给脚本 [game_event] handler（每帧）
         */
        void DispatchGameEventsToScript();

        /**
         * @brief 每帧调用脚本 [game_system] 低频决策（错峰调度）
         * @param dt 固定步长
         */
        void TickGameSystems(float dt);

        /**
         * @brief 加密 protobuf 消息并发送到客户端
         * @warning 必须在 LogicThread 中调用（独占 _sessions 读写权限）
         * @tparam TMsg protobuf 消息类型
         * @param sessionID  目标 Session
         * @param msgID      消息 ID（EMsgID）
         * @param msg        消息
         */
        template <typename TMsg>
        void SendToClient(uint32 sessionID, uint32 msgID, const TMsg &msg)
        {
            // 零分配序列化：ByteSizeLong → ByteBuffer::Own → SerializeToArray
            size_t bodySize = static_cast<size_t>(msg.ByteSizeLong());
            auto   buf      = ByteBuffer::Own(bodySize);
            bool   ok       = msg.SerializeToArray(buf.WritePtr(), static_cast<int>(bodySize));
            if (!ok)
            {
                Log::Error("SendToClient: SerializeToArray failed session={} msgID={}", sessionID, msgID);
                return;
            }
            buf.SetWritePos(bodySize);

            SendEncrypted(sessionID, msgID, buf.Data(), buf.Size());
        }

        // ── 消息分发（按 msgID 查表）──
        MessageDispatcher<uint32> _dispatcher; // context = sessionID

        // ── 组件 ──
        std::unique_ptr<IOContextPool>     _ioPool;
        std::unique_ptr<TCPAcceptor>       _gateAcceptor;
        std::unique_ptr<CenterClient>      _centerClient;
        std::unique_ptr<GateConnectionMgr> _gateConnMgr;
        LogicThread                        _logicThread;

        // ── Session 存储（IO 线程读锁 + LogicThread 独占写）──
        std::shared_mutex                        _sessionsMtx;
        std::unordered_map<uint32, WorldSession> _sessions;

        std::vector<std::unique_ptr<World>> _worlds;

        // 每 World 一个复制调度器（多场景各自独立打包发送）
        std::vector<std::unique_ptr<ReplicateScheduler>> _replicateSystems;

        // ── 游戏事件（ECS_06）──
        std::unique_ptr<GameEventBus> _gameEventBus;     // C++ 写，脚本读（每帧派发）
        float                         _accumTime = 0.0f; // 引擎累计时间（TickGameSystems 用）

        // ── 过载统计 ──
        size_t _prevQueueDepth = 0;

        // ── 配置 ──
        WorldConfig       _config;
        std::atomic<bool> _running {false};

        std::unique_ptr<IDasLangModuleProvider> _moduleProvider;
        // 脚本引擎实例（WorldServer 持有，显式注册进 DasLangEngine::SetInstance——
        // 替代 Meyers 单例的隐式全局状态）
        std::unique_ptr<DasLangEngine> _dasEngine;
    };

} // namespace MMO
