/**
 * @file WorldServer.cpp
 * @brief WorldServer 实现——Init/Run/Stop + LogicThread 回调
 */

#include "World/WorldServer.h"
#include "Common/Crypto/SessionToken.h"
#include "Common/Log/Log.h"
#include "Common/Network/TCPSocket.h"

#include <Internal/CenterRPC.pb.h>
#include <Internal/GateRPC.pb.h>
#include <Internal/InternalMsgID.pb.h>
#include <Login.pb.h>
#include <Move.pb.h>
#include <MsgID.pb.h>

#include <chrono>
#include <ctime>
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

        // 加载常驻场景
        if (!_sceneMgr.LoadPersistentScenes(cfg.world.persistentScenes))
        {
            Log::Error("WorldServer: no scenes loaded");
            return false;
        }

        // 注册消息分发
        RegisterHandlers();

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

        Log::Info("WorldServer: stopped");
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

    void WorldServer::OnTick([[maybe_unused]] std::chrono::milliseconds elapsed)
    {
        // 1. 处理未路由的 EnterWorldReq（内部持 unique_lock 写 _sessions）
        ProcessUnroutedMessages();

        // 2. 控制消息（DisconnectNtf / SessionRebindReq）
        ProcessControlMessages();

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

        // 4. 游戏逻辑（后续 ecs_stage）
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
                Log::Warn("WorldServer: unexpected unrouted msgID={}", msg.msgID);
                continue;
            }

            auto *scene = _sceneMgr.GetDefaultScene();
            if (!scene)
            {
                Log::Warn("WorldServer: no default scene for EnterWorld");
                continue;
            }

            // ProcessUnroutedMessages 会写 _sessions（insert），
            // 与 IO 线程的 shared_lock 读互斥，需持 unique_lock
            std::unique_lock lock(_sessionsMtx);
            EnterWorldHandler::Handle(
                msg.sessionID,
                msg.body.Data(),
                msg.body.Size(),
                _sessions,
                _config.security.loginServerSecret,
                1, // gateID（MVP：固定 1）
                *scene,
                [this](uint32 sessionID, uint32 msgID, ByteBuffer rawBody) {
                    // 出站：完成加密 + PacketHeader 包装后通过 GateConnectionMgr 发回客户端
                    auto it         = _sessions.find(sessionID);
                    bool hasSession = (it != _sessions.end());

                    // EnterWorldRsp 不走加密（客户端尚未初始化 CryptoSession）
                    if (msgID == Proto::MSG_LOGIN_ENTER_WORLD_RSP)
                    {
                        uint32     totalLen = static_cast<uint32>(sizeof(PacketHeader) + rawBody.Size());
                        ByteBuffer frame    = ByteBuffer::Own(totalLen);
                        frame.WriteUint32(totalLen);
                        frame.WriteUint32(msgID);
                        frame.WriteUint32(sessionID);
                        frame.WriteBytes(rawBody.Data(), rawBody.Size());
                        _gateConnMgr->SendToGate(1, sessionID, std::move(frame));
                        return;
                    }

                    if (hasSession)
                    {
                        auto encrypted = it->second.crypto.Encrypt(rawBody.Data(), rawBody.Size());
                        if (encrypted.Size() == 0)
                        {
                            Log::Warn("WorldServer: encrypt failed for session={}", sessionID);
                            return;
                        }

                        uint32     totalLen = static_cast<uint32>(sizeof(PacketHeader) + encrypted.Size());
                        ByteBuffer frame    = ByteBuffer::Own(totalLen);
                        frame.WriteUint32(totalLen);
                        frame.WriteUint32(msgID);
                        frame.WriteUint32(sessionID);
                        frame.WriteBytes(encrypted.Data(), encrypted.Size());

                        _gateConnMgr->SendToGate(it->second.gateServerID, sessionID, std::move(frame));
                    }
                    else
                    {
                        uint32     totalLen = static_cast<uint32>(sizeof(PacketHeader) + rawBody.Size());
                        ByteBuffer frame    = ByteBuffer::Own(totalLen);
                        frame.WriteUint32(totalLen);
                        frame.WriteUint32(msgID);
                        frame.WriteUint32(sessionID);
                        frame.WriteBytes(rawBody.Data(), rawBody.Size());
                        _gateConnMgr->SendToGate(1, sessionID, std::move(frame));
                    }
                });

            // 如果 Handle 执行后创建了新 session，通知 Center
            if (!_sessions.empty())
            {
                auto it = _sessions.find(msg.sessionID);
                if (it != _sessions.end() && !it->second.disconnected)
                {
                    NotifyCenterPlayerOnline(it->second.accountID);
                }
            }
        }
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
                Log::Info("WorldServer: disconnect timeout for account={}, destroying entity", accountID);

                ECS::Scene *scene = _sceneMgr.GetScene(it->second.entity.sceneId);
                if (scene)
                {
                    scene->DestroyEntity(it->second.entity);
                }

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

} // namespace MMO
