/**
 * @file CenterServer.cpp
 * @brief CenterServer 实现——内部 RPC handler 注册与分发
 */
#include "Center/CenterServer.h"
#include "Center/CenterConfig.h"

#include "Common/Log/Log.h"
#include "Common/Network/TCPSocket.h"

// 内部 RPC proto
#include <Internal/CenterRPC.pb.h>
#include <Internal/InternalMsgID.pb.h>

namespace MMO
{

    bool CenterServer::Init(const CenterConfig &cfg)
    {
        _ioPool = std::make_unique<IOContextPool>(static_cast<size_t>(cfg.network.ioThreads));
        // 内部 RPC 连接使用 LengthPrefix 帧协议
        _acceptor = std::make_unique<TCPAcceptor>(*_ioPool, cfg.network.port, EFraming::LengthPrefix);
        // 注册 RPC handler
        // ── 服务注册 ──
        _rpcHandlers.Register<Proto::Internal::MSG_REGISTER_WORLD_REQ, Proto::Internal::RegisterWorldReq>(
            [this](RPCContext ctx, const Proto::Internal::RegisterWorldReq &req) {
                OnRegisterWorld(std::move(ctx), req);
            });

        _rpcHandlers.Register<Proto::Internal::MSG_HEARTBEAT_REQ, Proto::Internal::HeartbeatReq>(
            [this](RPCContext ctx, const Proto::Internal::HeartbeatReq &req) {
                OnHeartbeat(std::move(ctx), req);
            });

        // ── 玩家位置 ──
        _rpcHandlers.Register<Proto::Internal::MSG_QUERY_PLAYER_LOCATION_REQ,
                              Proto::Internal::QueryPlayerLocationReq>(
            [this](RPCContext ctx, const Proto::Internal::QueryPlayerLocationReq &req) {
                OnQueryPlayerLocation(std::move(ctx), req);
            });

        _rpcHandlers.Register<Proto::Internal::MSG_PLAYER_ONLINE_NTF, Proto::Internal::PlayerOnlineNtf>(
            [this](RPCContext ctx, const Proto::Internal::PlayerOnlineNtf &req) {
                OnPlayerOnline(std::move(ctx), req);
            });

        _rpcHandlers.Register<Proto::Internal::MSG_PLAYER_OFFLINE_NTF, Proto::Internal::PlayerOfflineNtf>(
            [this](RPCContext ctx, const Proto::Internal::PlayerOfflineNtf &req) {
                OnPlayerOffline(std::move(ctx), req);
            });

        // ── World 重连重建 ──
        _rpcHandlers
            .Register<Proto::Internal::MSG_BATCH_PLAYERS_ONLINE_NTF, Proto::Internal::BatchOnlinePlayersNtf>(
                [this](RPCContext ctx, const Proto::Internal::BatchOnlinePlayersNtf &req) {
                    OnBatchOnlinePlayers(std::move(ctx), req);
                });

        // ── 负载均衡 ──
        _rpcHandlers.Register<Proto::Internal::MSG_PICK_WORLD_REQ, Proto::Internal::PickWorldReq>(
            [this](RPCContext ctx, const Proto::Internal::PickWorldReq &req) {
                OnPickWorld(std::move(ctx), req);
            });

        // acceptor 回调：设置 MessageHandler → Start
        _acceptor->Start([this](std::shared_ptr<TCPSocket> socket) {
            OnNewConnection(std::move(socket));
        });

        Log::Info("CenterServer listening on port {}", cfg.network.port);
        return true;
    }

    void CenterServer::Run()
    {
        _running = true;
        _ioPool->Start();

        StartHealthCheck();

        Log::Info("CenterServer: running");
        _ioPool->Wait(); // 主线程阻塞，直到 Stop() 被调用
    }

    void CenterServer::Stop()
    {
        _running = false;
        if (_healthTimer)
        {
            _healthTimer->cancel();
        }
        if (_acceptor)
        {
            _acceptor->Stop();
        }
        if (_ioPool)
        {
            _ioPool->Stop();
        }
    }

    void CenterServer::OnNewConnection(std::shared_ptr<TCPSocket> socket)
    {
        auto sockPtr = socket.get();

        // 设置 OnClose 回调：断线时自动从服务注册表移除
        socket->SetCloseHandler([this, sockPtr]() {
            auto it = _socketToService.find(sockPtr);
            if (it != _socketToService.end())
            {
                Log::Info("CenterServer: socket lost for service '{}'", it->second);
                _services.OnSocketLost(it->second);
                _socketToService.erase(it);
            }
        });

        socket->SetMessageHandler([this, sock = std::move(socket)](uint32 msgID,
                                                                   uint32 /*sessionID*/,
                                                                   const uint8 *body,
                                                                   size_t       len) mutable {
            // LengthPrefix 帧模式下 msgID/sessionID 为 0，
            // 需要解析 RPCHeader 获取真实 msgID 和 requestID
            if (len < sizeof(RPCHeader))
            {
                Log::Error("CenterServer: received undersized RPC frame ({} bytes)", len);
                return;
            }

            // 解析 RPCHeader（大端序）
            auto   headerBuf = ByteBuffer::Wrap(body, sizeof(RPCHeader));
            uint32 rpcMsgID  = headerBuf.ReadUint32();
            uint64 requestID = headerBuf.ReadUint64();
            /*uint64 traceID =*/headerBuf.ReadUint64();
            uint8 type = headerBuf.ReadUint8();

            size_t       protoLen  = len - sizeof(RPCHeader);
            const uint8 *protoData = body + sizeof(RPCHeader);

            if (static_cast<ERPCType>(type) == ERPCType::Request)
            {
                RPCContext ctx;
                ctx.requestID = requestID;
                ctx.msgID     = rpcMsgID;
                ctx.socket    = sock;

                if (!_rpcHandlers.Dispatch(ctx, rpcMsgID, protoData, protoLen))
                {
                    Log::Warn("CenterServer: unregistered RPC handler for msgID {}", rpcMsgID);
                }
            }
            else if (static_cast<ERPCType>(type) == ERPCType::Notify)
            {
                RPCContext ctx;
                ctx.requestID = 0;
                ctx.msgID     = rpcMsgID;
                ctx.socket    = sock;

                if (!_rpcHandlers.Dispatch(ctx, rpcMsgID, protoData, protoLen))
                {
                    Log::Warn("CenterServer: unregistered RPC NTF handler for msgID {}", rpcMsgID);
                }
            }
            // Response 类型理论上不由 Center 接收（Center 不做发起方），丢弃
        });

        sockPtr->Start();
    }

    // ── RPC handler 实现 ──

    void CenterServer::OnRegisterWorld(RPCContext ctx, const Proto::Internal::RegisterWorldReq &req)
    {
        ServiceRegistry::ServiceInfo info;
        info.serviceID      = req.service_id();
        info.address        = req.address();
        info.maxPlayers     = req.max_players();
        info.currentPlayers = 0;

        _services.Register(info);

        // 记录 socket → serviceID 映射，用于断线感知
        _socketToService[ctx.socket.get()] = req.service_id();

        Log::Info("CenterServer: World '{}' registered (address={}, maxPlayers={})",
                  req.service_id(),
                  req.address(),
                  req.max_players());

        Proto::Internal::RegisterWorldRsp rsp;
        rsp.set_ok(true);
        ctx.Reply(rsp);
    }

    void CenterServer::OnHeartbeat(RPCContext ctx, const Proto::Internal::HeartbeatReq &req)
    {
        // 从 socket 映射查找 serviceID
        auto it = _socketToService.find(ctx.socket.get());
        if (it == _socketToService.end())
        {
            Log::Warn("CenterServer: heartbeat from unregistered socket");
            return;
        }

        _services.Heartbeat(it->second, req.current_players());

        Proto::Internal::HeartbeatRsp rsp;
        rsp.set_ok(true);
        ctx.Reply(rsp);
    }

    void CenterServer::OnPickWorld(RPCContext ctx, const Proto::Internal::PickWorldReq & /*req*/)
    {
        auto *best = _services.PickLeastLoadedWorld();

        Proto::Internal::PickWorldRsp rsp;
        if (best)
        {
            rsp.set_world_server_id(best->serviceID);
            rsp.set_address(best->address);
        }
        // else: 空字符串，LoginServer 处理为无可用 World
        ctx.Reply(rsp);
    }

    void CenterServer::OnQueryPlayerLocation(RPCContext                                     ctx,
                                             const Proto::Internal::QueryPlayerLocationReq &req)
    {
        auto serviceID = _playerIndex.GetServiceID(req.account_id());

        Proto::Internal::QueryPlayerLocationRsp rsp;
        if (serviceID)
        {
            rsp.set_world_server_id(*serviceID);
        }
        // else: 空字符串，表示玩家不在线
        ctx.Reply(rsp);
    }

    void CenterServer::OnPlayerOnline(RPCContext ctx, const Proto::Internal::PlayerOnlineNtf &req)
    {
        _playerIndex.RegisterPlayer(req.account_id(), req.world_server_id());
        Log::Debug("CenterServer: player {} online on '{}'", req.account_id(), req.world_server_id());
    }

    void CenterServer::OnPlayerOffline(RPCContext ctx, const Proto::Internal::PlayerOfflineNtf &req)
    {
        _playerIndex.UnregisterPlayer(req.account_id());
        Log::Debug("CenterServer: player {} offline", req.account_id());
    }

    void CenterServer::OnBatchOnlinePlayers(RPCContext ctx, const Proto::Internal::BatchOnlinePlayersNtf &req)
    {
        auto worldID = std::to_string(req.world_server_id());
        _playerIndex.ClearWorld(worldID);
        for (uint32 accountID : req.account_ids())
        {
            _playerIndex.RegisterPlayer(accountID, worldID);
        }
        Log::Info("CenterServer: World {} replayed {} online players",
                  req.world_server_id(),
                  req.account_ids_size());
    }

    // ── 心跳超时检查 ──

    void CenterServer::StartHealthCheck()
    {
        if (_healthTimer)
        {
            return;
        }

        _healthTimer = std::make_unique<asio::steady_timer>(_ioPool->GetNextContext());
        _healthTimer->expires_after(std::chrono::seconds(10));
        _healthTimer->async_wait([this](const asio::error_code &ec) {
            if (ec || !_running.load(std::memory_order_acquire))
            {
                return;
            }
            CheckServiceHealth();
            // 重新调度（非递归，MSVC lambda 友好）
            _healthTimer->expires_after(std::chrono::seconds(10));
            _healthTimer->async_wait([this](const asio::error_code &ec2) {
                if (ec2 || !_running.load(std::memory_order_acquire))
                {
                    return;
                }
                CheckServiceHealth();
                StartHealthCheck();
            });
        });
    }

    void CenterServer::CheckServiceHealth()
    {
        _services.CheckTimeouts();
    }

} // namespace MMO
