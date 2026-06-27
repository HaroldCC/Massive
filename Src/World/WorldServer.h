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

#include "Common/Network/IOContextPool.h"
#include "Common/Network/MessageDispatcher.h"
#include "Common/Network/TCPAcceptor.h"

#include "World/CenterClient.h"
#include "World/GateConnection.h"
#include "World/Handler/EnterWorldHandler.h"
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
        ECS::Scene _defaultScene {1};  // 默认场景（id=1）

        // ── 配置 ──
        WorldConfig _config;
        std::atomic<bool> _running {false};
    };

} // namespace MMO
