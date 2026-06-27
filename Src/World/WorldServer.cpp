/**
 * @file WorldServer.cpp
 * @brief WorldServer 实现——Init/Run/Stop + LogicThread 回调
 */

#include "World/WorldServer.h"
#include "Common/Crypto/SessionToken.h"
#include "Common/Log/Log.h"
#include "Common/Network/TCPSocket.h"

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace MMO
{

    // ── Init ──

    bool WorldServer::Init(const WorldConfig &cfg)
    {
        _config = cfg;

        _ioPool = std::make_unique<IOContextPool>(static_cast<size_t>(cfg.network.ioThreads));

        if (!InitCenterClient(cfg))
        {
            Log::Error("WorldServer: CenterClient init failed");
            return false;
        }

        if (!InitGateAcceptor(cfg))
        {
            Log::Error("WorldServer: GateAcceptor init failed");
            return false;
        }

        // 注册 LogicThread 回调
        _gateConnMgr->SetSessionsPtr(&_sessionsMtx, &_sessions);

        _logicThread.Start(
            &_sessions, &_sessionsMtx,
            [this](auto elapsed) { OnTick(elapsed); },
            [this](auto sessionID, auto &ws, const auto &msg) { OnMessage(sessionID, ws, msg); },
            [this]() { OnPreProcess(); },
            [this]() { OnPostFlush(); });

        Log::Info("WorldServer: initialized (worldID={}, port={})",
                  cfg.world.worldServerID, cfg.network.internalPort);
        return true;
    }

    bool WorldServer::InitCenterClient(const WorldConfig &cfg)
    {
        _centerClient = std::make_unique<CenterClient>();

        std::string address = cfg.center.host + ":" + std::to_string(cfg.center.port);

        return _centerClient->Connect(
            cfg.center.host,
            cfg.center.port,
            cfg.world.worldServerID,
            cfg.world.maxPlayers,
            address,
            _ioPool.get());
    }

    bool WorldServer::InitGateAcceptor(const WorldConfig &cfg)
    {
        _gateConnMgr = std::make_unique<GateConnectionMgr>();

        // 监听 Gate 内部连接（LengthPrefix 帧协议）
        _gateAcceptor = std::make_unique<TCPAcceptor>(*_ioPool, cfg.network.internalPort, Framing::LengthPrefix);

        // 使用一个简单的 gateID 自增计数器（MVP：单个 Gate 连接）
        static std::atomic<uint16> nextGateID {1};

        _gateAcceptor->Start([this](std::shared_ptr<TCPSocket> socket) {
            uint16 gateID = nextGateID.fetch_add(1, std::memory_order_relaxed);
            _gateConnMgr->AcceptConnection(gateID, std::move(socket));
        });

        Log::Info("WorldServer: listening for Gate connections on port {}", cfg.network.internalPort);
        return true;
    }

    // ── Run / Stop ──

    void WorldServer::Run()
    {
        _running.store(true, std::memory_order_release);
        _ioPool->Start();

        Log::Info("WorldServer: running");

        // 主线程等待 LogicThread 运行
        while (_running.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(100ms);
        }
    }

    void WorldServer::Stop()
    {
        _running.store(false, std::memory_order_release);
        _logicThread.Stop();

        if (_gateAcceptor)
        {
            _gateAcceptor->Stop();
        }
        if (_ioPool)
        {
            _ioPool->Stop();
        }

        Log::Info("WorldServer: stopped");
    }

    // ── LogicThread 回调 ──

    void WorldServer::OnTick(std::chrono::milliseconds elapsed)
    {
        // 游戏逻辑入口（DasLang 脚本）
        // MVP: 空实现，后续通过 ecs_stage 组织各 System
        (void)elapsed;
    }

    void WorldServer::OnMessage(uint32 sessionID, WorldSession &ws, const LogicMessage &msg)
    {
        (void)ws;
        // LogicThread 独占，按 msgID 查表分发
        auto dispatched = _dispatcher.Dispatch(sessionID, msg.msgID, msg.body.Data(), msg.body.Size());
        if (!dispatched)
        {
            Log::Debug("WorldServer: unhandled msgID={} from session={}", msg.msgID, sessionID);
        }
    }

    void WorldServer::OnPreProcess()
    {
        if (_centerClient)
        {
            // RPC 超时检查
            _centerClient->GetRPCClient().ProcessTimeouts();
        }
    }

    void WorldServer::OnPostFlush()
    {
        if (_centerClient && _centerClient->IsConnected())
        {
            // MVP: 每 Tick 都发心跳（后续用 TimingWheel 替代）
            static int tickCounter = 0;
            if (++tickCounter % (1000 / 20) == 0) // 每 ~1s
            {
                uint32 online = static_cast<uint32>(_sessions.size());
                _centerClient->SendHeartbeat(online);
            }
        }
    }

} // namespace MMO
