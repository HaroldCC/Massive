/**
 * @file WorldServer.cpp
 * @brief WorldServer 实现——Init/Run/Stop + LogicThread 回调
 */

#include "World/WorldServer.h"
#include "Common/Log/Log.h"
#include "Common/Network/TCPSocket.h"
#include "ScriptEngine/DasEngine.h"
#include "ScriptEngine/DasEngineConfig.h"
#include "World/DasModule/WorldDasModule.h"
#include "World/AutoGen/ProtoBindIndex.gen.h"
#include "World/Handler/EnterWorldHandler.h"

#include "Internal/CenterRPC.pb.h"
#include "Internal/GateRPC.pb.h"
#include "Internal/InternalMsgID.pb.h"
#include "Move.pb.h"
#include "MsgID.pb.h"
#include "daScript/misc/sysos.h"

#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

namespace MMO
{

    // ── Init ──

    bool WorldServer::Init(const WorldConfig &cfg)
    {
        _config = cfg;

        // CWD 由 xmake set_rundir / systemd WorkingDirectory 保证为项目根
        // dasRoot 由 daScript 内部用于 daslib/modules//%/ 路径解析
        das::setDasRoot(_config.script.dasRoot);

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

        // 创建常驻场景对应的 World
        InitWorlds();

        // 游戏事件总线（C++ 写 → 脚本 [game_event] 读）
        _gameEventBus = std::make_unique<GameEventBus>();

        // 每个 World 一个复制调度器（多场景各自独立打包，不再钉死 _worlds[0]）
        _replicateSystems.clear();
        _replicateSystems.reserve(_worlds.size());
        for (auto &world : _worlds)
        {
            _replicateSystems.emplace_back(std::make_unique<ReplicateScheduler>(
                world->GetScene(),
                [this](uint32 sessionID, const uint8 *data, size_t len) {
                    SendEncrypted(sessionID, Proto::MSG_ENTITY_REPLICATE_NTF, data, len);
                }));
        }

        // 注册消息分发
        RegisterHandlers();

        // 实例化脚本模块提供者（World 专用 das 模块：消息类型 + EMsgID 绑定）
        _moduleProvider = std::make_unique<WorldDasModule>();

        // 初始化脚本引擎（Phase 1：最小 DaLang + DECS 验证）
        if (!InitScriptEngine())
        {
            Log::Error("WorldServer: script engine init failed");
            return false;
        }

        // 注册 Gate 指向 _sessions
        _gateConnMgr->SetSessionsPtr(&_sessionsMtx, &_sessions);

        _logicThread.Start(
            &_sessions,
            &_sessionsMtx,
            [this](auto elapsed) {
                OnTick(elapsed);
            },
            [this](auto sessionID, auto &ws, const auto &msg) {
                OnMessage(sessionID, ws, msg);
            },
            [this]() {
                OnPreProcess();
            },
            [this]() {
                OnPostFlush();
            });

        Log::Info("WorldServer: initialized (worldID={}, port={})",
                  cfg.world.worldServerID,
                  cfg.network.internalPort);
        return true;
    }

    bool WorldServer::InitCenterClient(const WorldConfig &cfg)
    {
        _centerClient = std::make_unique<CenterClient>();

        std::string address = cfg.center.host + ":" + std::to_string(cfg.center.port);

        // 注册在线玩家收集器：Center 重连后自动 dump 全量在线列表重建索引
        _centerClient->SetOnlinePlayersCollector([this]() -> std::vector<uint32> {
            std::shared_lock    lock(_sessionsMtx);
            std::vector<uint32> ids;
            ids.reserve(_sessions.size());
            for (const auto &[sessionID, ws] : _sessions)
            {
                ids.push_back(ws.accountID);
            }
            return ids;
        });

        return _centerClient->Connect(cfg.center.host,
                                      cfg.center.port,
                                      cfg.world.worldServerID,
                                      cfg.world.maxPlayers,
                                      address,
                                      _ioPool.get());
    }

    void WorldServer::InitWorlds()
    {
        _worlds.clear();
        for (uint32 sceneID : _config.world.persistentScenes)
        {
            auto world = std::make_unique<World>(static_cast<uint16>(sceneID));
            world->Init();
            _worlds.push_back(std::move(world));
            Log::Info("WorldServer: world scene={} initialized", sceneID);
        }
        Log::Info("WorldServer: {} worlds initialized", _worlds.size());
    }

    bool WorldServer::InitGateAcceptor(const WorldConfig &cfg)
    {
        _gateConnMgr = std::make_unique<GateConnectionMgr>();

        // 监听 Gate 内部连接（LengthPrefix 帧协议）
        _gateAcceptor =
            std::make_unique<TCPAcceptor>(*_ioPool, cfg.network.internalPort, EFraming::LengthPrefix);

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

        // 关闭脚本引擎（模块归 daScript 环境管理，Shutdown 统一释放）
        if (_dasEngine)
        {
            _dasEngine->Shutdown();
        }
        _dasEngine.reset();

        Log::Info("WorldServer: stopped");
    }

    void WorldServer::SendRawToClient(uint32 sessionID, uint32 msgID, const uint8 *data, size_t len)
    {
        // IDasLangHost 接口：脚本侧原始发送（暂未用，走 SendEncrypted）
        SendEncrypted(sessionID, msgID, data, len);
    }

    void WorldServer::SendEncrypted(uint32 sessionID, uint32 msgID, const uint8 *data, size_t len)
    {
        // 复制系统/脚本侧产出的已序列化字节 → 加密 + PacketHeader 包装 → Gate
        auto it = _sessions.find(sessionID);
        if (it == _sessions.end())
        {
            Log::Warn("SendEncrypted: session {} not found", sessionID);
            return;
        }

        // 加密 = [Seq:4B][Ciphertext+Tag]
        auto encrypted = it->second.crypto.Encrypt(data, len);
        if (encrypted.Size() == 0)
        {
            Log::Warn("SendEncrypted: encrypt failed session={}", sessionID);
            return;
        }

        // 构建完整包: [PacketHeader:12B][encrypted]
        uint32 totalLen = static_cast<uint32>(sizeof(PacketHeader) + encrypted.Size());
        auto   frame    = ByteBuffer::Own(totalLen);
        frame.WriteUint32(totalLen); // PacketHeader.length
        frame.WriteUint32(msgID);
        frame.WriteUint32(sessionID);
        frame.WriteBytes(encrypted.Data(), encrypted.Size());

        _gateConnMgr->SendToGate(it->second.gateServerID, sessionID, std::move(frame));
    }

    GameEventBus *WorldServer::GetGameEventBus()
    {
        return _gameEventBus.get();
    }

    ECS::Scene *WorldServer::GetDefaultScene()
    {
        return _worlds.empty() ? nullptr : &_worlds[0]->GetScene();
    }

    ECS::Scene *WorldServer::GetSceneByEntityID(uint64 entityID)
    {
        // 单场景 MVP：scene 位匹配才返回
        const uint16 sceneID = ECS::SceneOf(ECS::EntityID(entityID));
        for (auto &world : _worlds)
        {
            if (world->GetSceneID() == sceneID)
            {
                return &world->GetScene();
            }
        }
        return nullptr;
    }

    /**
     * @brief 把事件总线内容派发给脚本 [game_event] handler
     *
     * 流程：GameEventBus::Drain() → 逐事件 eval DispatchGameEvent(type, payload)
     * 每个事件一次脚本调用（铁律 2：事件数有限）
     */
    void WorldServer::DispatchGameEventsToScript()
    {
        if (!_gameEventBus)
        {
            return;
        }

        auto &dasEngine = DasLangEngine::GetIns();
        auto *ctx       = dasEngine.GetScriptContext();
        if (!ctx)
        {
            return;
        }
        auto *fnDispatch = ctx->findFunction("DispatchGameEvent");
        if (!fnDispatch)
        {
            return;
        }

        auto events = _gameEventBus->Drain();
        if (events.empty())
        {
            return;
        }

        // 逐事件派发：DispatchGameEvent(evType, payloadPtr)
        // CallScriptFunctionIn 校验 arity（脚本 expects 2 参）——签名漂移立即报错
        for (auto &env : events)
        {
            if (!DasLangEngine::CallScriptFunctionIn(ctx,
                                                     fnDispatch,
                                                     "DispatchGameEvent",
                                                     env.event_type,
                                                     const_cast<uint8_t *>(env.data)))
            {
                break; // 脚本异常：停止本帧剩余事件（避免级联）
            }
        }
    }

    /**
     * @brief 每帧调用脚本 [game_system] 低频决策（错峰调度）
     */
    void WorldServer::TickGameSystems(float dt)
    {
        auto &dasEngine = DasLangEngine::GetIns();
        auto *ctx       = dasEngine.GetScriptContext();
        if (!ctx)
        {
            return;
        }
        auto *fnTick = ctx->findFunction("TickGameSystems");
        if (!fnTick)
        {
            Log::Warn("TickGameSystems: function not found (script may lack GameSystemRegistry require)");
            return;
        }

        _accumTime += dt;
        if (!DasLangEngine::CallScriptFunctionIn(ctx, fnTick, "TickGameSystems", dt, _accumTime))
        {
            Log::Warn("TickGameSystems: script call failed (arity mismatch or exception)");
        }
    }

    // ── 消息分发注册 ──

    void WorldServer::RegisterHandlers()
    {
        // EnterWorldReq 由 OnTick 中消费 _unroutedQueue 直接处理

        // MoveReq
        _dispatcher.Register<Proto::MSG_MOVE_REQ, Proto::MoveReq>(
            [this](uint32 sessionID, const Proto::MoveReq &req) {
                auto it = _sessions.find(sessionID);
                if (it == _sessions.end())
                {
                    return;
                }

                // 速度校验（服务器权威）
                if (req.speed() < 0.0f || req.speed() > 50.0f)
                {
                    Log::Debug("MoveHandler: invalid speed={} session={}", req.speed(), sessionID);
                    return;
                }

                Log::Debug("MoveHandler: session={} pos=({:.1f},{:.1f},{:.1f})",
                           sessionID,
                           req.position().x(),
                           req.position().y(),
                           req.position().z());

                // 回包
                auto nowMs = static_cast<uint32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                     std::chrono::system_clock::now().time_since_epoch())
                                                     .count());

                Proto::MoveRsp rsp;
                rsp.set_sequence(req.sequence());
                rsp.mutable_position()->CopyFrom(req.position());
                rsp.set_server_time(nowMs);

                // 加密出站
                SendToClient(sessionID, Proto::MSG_MOVE_RSP, rsp);
            });
    }

    // ── LogicThread 回调 ──

    void WorldServer::OnTick(float dt)
    {
        // 1. 处理未路由的 EnterWorldReq（内部持 unique_lock 写 _sessions）
        ProcessUnroutedMessages();

        // 2. 控制消息（DisconnectNtf / SessionRebindReq）
        ProcessControlMessages();

        for (auto &&world : _worlds)
        {
            world->Tick(dt);
        }

        // 3. 脚本引擎驱动：热重载轮询（PollReload）+ 脚本 Update + GC
        //    这是 DasLangEngine::Tick 的唯一调用点——不调用则热重载标志永不消费、
        //    funcUpdate 永不执行、collectHeap GC 永不触发（曾长期断链，见 ECS 审查）。
        DasLangEngine::GetIns().Tick(dt);

        // 脚本低频决策（[game_system] 错峰调度）
        TickGameSystems(dt);

        // 游戏事件派发（[game_event] 类型化分发）
        DispatchGameEventsToScript();

        // 复制阶段：每个 World 消费自身 AOI 状态 → 打包 → 加密发送
        for (size_t i = 0; i < _worlds.size() && i < _replicateSystems.size(); ++i)
        {
            auto &world = *_worlds[i];
            auto &repl  = *_replicateSystems[i];
            repl.Update(world.GetAoiState(), world.GetAoiEnters(), world.GetAoiLeaves());
            // 事件消费后清空（AOI 阶段已填充，复制阶段清掉）
            world.GetAoiEnters().clear();
            world.GetAoiLeaves().clear();
        }

        // 3. 过载保护（读遍历 _sessions，与 IO 线程 shared_lock 并发读不冲突）
        //    但此处 LogicThread 刚写完 _sessions（ProcessUnroutedMessages），
        //    后续无写，读锁只是为了与 IO 线程互斥
        size_t queueDepth = 0;
        {
            std::shared_lock lock(_sessionsMtx);
            for (auto &[sid, ws] : _sessions)
            {
                queueDepth += ws.inbox.SizeApprox();
            }
        }
        UpdateLoadLevel(_sessions.size(), queueDepth);
    }

    void WorldServer::ProcessUnroutedMessages()
    {
        auto &unrouted = _gateConnMgr->GetUnroutedQueue();

        std::vector<LogicMessage> batch;
        unrouted.DrainAll(batch);

        for (auto &msg : batch)
        {
            if (msg.msgID != Proto::MSG_LOGIN_ENTER_WORLD_REQ)
            {
                Log::Warn("WorldServer: unrouted msgID={} from session={} ignored", msg.msgID, msg.sessionID);
                continue;
            }

            auto *scene = _worlds.empty() ? nullptr : &_worlds[0]->GetScene();
            if (nullptr == scene)
            {
                Log::Warn("WorldServer: default scene not found, cannot process EnterWorldReq for session={}",
                          msg.sessionID);
                continue;
            }

            std::unique_lock lock(_sessionsMtx);
            EnterWorldHandler::Handle(
                msg.sessionID,
                msg.body.Data(),
                msg.body.Size(),
                _sessions,
                _config.security.loginServerSecret,
                1,
                *scene,
                [this](uint32 sessionID, uint32 msgID, ByteBuffer body) {
                    auto it = _sessions.find(sessionID);
                    if (msgID == Proto::MSG_LOGIN_ENTER_WORLD_RSP)
                    {
                        uint32 totalLen = static_cast<uint32>(sizeof(PacketHeader) + body.Size());
                        auto   frame    = ByteBuffer::Own(totalLen);
                        frame.WriteUint32(totalLen); // PacketHeader.length
                        frame.WriteUint32(msgID);
                        frame.WriteUint32(sessionID);
                        frame.WriteBytes(body.Data(), body.Size());
                        _gateConnMgr->SendToGate(it->second.gateServerID, sessionID, std::move(frame));
                        return;
                    }
                });
        }
    }

    void WorldServer::OnMessage(uint32 sessionID, WorldSession &ws, const LogicMessage &msg)
    {
        // 优先尝试脚本分发
        if (DasLangEngine::GetIns().DispatchRegistry().Dispatch(sessionID,
                                                                msg.msgID,
                                                                msg.body.Data(),
                                                                msg.body.Size()))
        {
            return;
        }

        // 未注册进 ScriptDispatchRegistry 的消息（控制消息、尚未迁移的消息）→ C++ 分发
        auto dispatched = _dispatcher.Dispatch(sessionID, msg.msgID, msg.body.Data(), msg.body.Size());
        if (!dispatched)
        {
            Log::Debug("WorldServer: unhandled msgID={} from session={}", msg.msgID, sessionID);
        }
    }

    void WorldServer::OnPreProcess()
    {
        // RPC 超时检查已移至 RPCClient IO 线程定时器，这里不再需要
    }

    void WorldServer::OnPostFlush()
    {
        // 排干 RPCClient 已完成回调队列（超时/响应回调）
        if (_centerClient)
        {
            _centerClient->GetRPCClient().DrainCompleted();
        }

        if (_centerClient && _centerClient->IsConnected())
        {
            static int tickCounter = 0;
            if (++tickCounter % (1000 / 20) == 0) // 每 ~1s
            {
                uint32 online;
                {
                    std::shared_lock lock(_sessionsMtx);
                    online = static_cast<uint32>(_sessions.size());
                }
                _centerClient->SendHeartbeat(online);
            }
        }
    }

    // ── 控制消息处理（LogicThread 消费 _ctrlQueue）──

    void WorldServer::ProcessControlMessages()
    {
        auto &ctrlQueue = _gateConnMgr->GetCtrlQueue();

        std::vector<LogicMessage> batch;
        ctrlQueue.DrainAll(batch);

        for (auto &msg : batch)
        {
            switch (msg.msgID)
            {
                case Proto::Internal::MSG_DISCONNECT_NTF:
                {
                    Proto::Internal::DisconnectNtf ntf;
                    if (ntf.ParseFromArray(msg.body.Data(), static_cast<int>(msg.body.Size())))
                    {
                        OnDisconnectNtf(ntf.session_id());
                    }
                    break;
                }
                case Proto::Internal::MSG_SESSION_REBIND_REQ:
                    OnSessionRebindReq(msg.body.Data(), msg.body.Size());
                    break;
                default:
                    Log::Debug("WorldServer: unhandled control msgID={}", msg.msgID);
                    break;
            }
        }
    }

    void WorldServer::OnDisconnectNtf(uint32 sessionID)
    {
        std::shared_lock lock(_sessionsMtx);
        auto             it = _sessions.find(sessionID);
        if (it == _sessions.end())
        {
            return;
        }

        it->second.disconnected = true;
        uint32 accountID        = it->second.accountID;

        Log::Info("WorldServer: client disconnected session={} account={}", sessionID, accountID);

        // 注册 30s 超时定时器（LogicThread 独占，TimingWheel 由 RunLoop 驱动 Tick）
        _logicThread.GetTimingWheel().Schedule(std::chrono::seconds(30), [this, accountID]() {
            OnDisconnectTimeout(accountID);
        });
    }

    void WorldServer::OnDisconnectTimeout(uint32 accountID)
    {
        // 超时回调在 LogicThread 中执行（TimingWheel.Tick 触发），
        // 与 IO 线程 shared_lock 读互斥，需持 unique_lock（可能 erase）
        std::unique_lock lock(_sessionsMtx);
        for (auto it = _sessions.begin(); it != _sessions.end(); ++it)
        {
            if (it->second.accountID == accountID && it->second.disconnected)
            {
                Log::Info("WorldServer: disconnect timeout for account={}, removing session", accountID);

                // 通知 Center 玩家离线
                NotifyCenterPlayerOffline(accountID);

                _sessions.erase(it);
                return;
            }
        }
    }

    void WorldServer::OnSessionRebindReq(const uint8 *data, size_t len)
    {
        Proto::Internal::SessionRebindReq req;
        if (!req.ParseFromArray(data, static_cast<int>(len)))
        {
            return;
        }

        // MVP: Gate 批量通知 session 已恢复
        for (int i = 0; i < req.session_ids_size(); ++i)
        {
            uint32 sid = req.session_ids(i);
            auto   it  = _sessions.find(sid);
            if (it != _sessions.end())
            {
                it->second.disconnected = false;
                Log::Debug("WorldServer: session {} rebound (reconnected)", sid);
            }
        }
    }

    // ── Center 通知 ──

    void WorldServer::NotifyCenterPlayerOnline(uint32 accountID)
    {
        if (!_centerClient || !_centerClient->IsConnected())
        {
            return;
        }

        Proto::Internal::PlayerOnlineNtf ntf;
        ntf.set_account_id(accountID);
        ntf.set_world_server_id(std::to_string(_config.world.worldServerID));

        _centerClient->GetRPCClient().Notify(_centerClient->GetSocket(),
                                             Proto::Internal::MSG_PLAYER_ONLINE_NTF,
                                             ntf);
    }

    void WorldServer::NotifyCenterPlayerOffline(uint32 accountID)
    {
        if (!_centerClient || !_centerClient->IsConnected())
        {
            return;
        }

        Proto::Internal::PlayerOfflineNtf ntf;
        ntf.set_account_id(accountID);

        _centerClient->GetRPCClient().Notify(_centerClient->GetSocket(),
                                             Proto::Internal::MSG_PLAYER_OFFLINE_NTF,
                                             ntf);
    }

    // ── 过载保护 ──

    void WorldServer::UpdateLoadLevel(size_t sessionCount, size_t pendingMessages)
    {
        static constexpr size_t kWarnSessionCount    = 5000;
        static constexpr size_t kWarnPendingMessages = 10000;
        static constexpr size_t kDegradeSessionCount = 8000;
        static constexpr size_t kDegradePendingMsgs  = 20000;

        ELoadLevel oldLevel = _loadLevel;

        if (sessionCount >= kDegradeSessionCount || pendingMessages >= kDegradePendingMsgs)
        {
            _loadLevel = ELoadLevel::DEGRADED;
        }
        else if (sessionCount >= kWarnSessionCount || pendingMessages >= kWarnPendingMessages)
        {
            _loadLevel = ELoadLevel::WARNING;
        }
        else
        {
            _loadLevel = ELoadLevel::NORMAL;
        }

        if (oldLevel != _loadLevel)
        {
            ApplyLoadLevel(oldLevel, _loadLevel);
        }
    }

    void WorldServer::ApplyLoadLevel(ELoadLevel oldLevel, ELoadLevel newLevel)
    {
        (void)oldLevel;
        if (newLevel == ELoadLevel::DEGRADED)
        {
            Log::Error("WorldServer: DEGRADED — stopping new enters");
        }
        else if (newLevel == ELoadLevel::WARNING)
        {
            Log::Warn("WorldServer: WARNING — sessions={}", _sessions.size());
        }
    }

    // ── 脚本引擎 ──

    bool WorldServer::InitScriptEngine()
    {
        Log::Info("InitScriptEngine: starting...");

        // 引擎实例由 WorldServer 持有并显式注册（替代 Meyers 单例的隐式全局状态）
        _dasEngine = std::make_unique<DasLangEngine>();
        DasLangEngine::SetInstance(*_dasEngine);
        auto &dasEngine = *_dasEngine;

        DasLangEngineConfig cfg;
        cfg.dasLangRoot = _config.script.dasRoot;
        // .das_project 项目文件——get_file_access 编译它并启用 module_get 等回调（模块解析/沙箱）。
        // 相对项目根（CWD）。World 服务入口所在目录 Script/World/。
        cfg.projectFile = "Script/World/.das_project";

#ifdef DEBUG
        cfg.mode = EScriptMode::Develop;
#else
        cfg.mode = EScriptMode::Release;
#endif

        if (!dasEngine.Initialize(cfg, this, _moduleProvider.get()))
        {
            return false;
        }

        if (!dasEngine.Load("Script/World/main.das"))
        {
            Log::Error("Load Script/World/main.das fail:{}", dasEngine.GetLastErrors());
            return false;
        }

        RegisterAllMsgDispatch();

        Log::Info("InitScriptEngine: OK");
        return true;
    }

} // namespace MMO
