/**
 * @file CenterClient.cpp
 * @brief CenterClient 实现
 */

#include "World/CenterClient.h"
#include "Common/Log/Log.h"

#include <Internal/CenterRPC.pb.h>
#include <Internal/InternalMsgID.pb.h>

namespace MMO
{

    CenterClient::CenterClient() : _rpcClient(true) // LogicThread 模式
    {
    }

    bool CenterClient::Connect(const std::string &host,
                               uint16             port,
                               uint16             worldServerID,
                               uint16             maxPlayers,
                               const std::string &address,
                               IOContextPool     *ioPool)
    {
        _worldServerID = worldServerID;
        _maxPlayers    = maxPlayers;
        _address       = address;
        _host          = host;
        _port          = port;
        _ioPool        = ioPool;

        // 在 IO 线程启动 RPC 超时检查定时器
        _rpcClient.StartTimeoutChecker(ioPool->GetNextContext());

        DoConnect();

        return true;
    }

    void CenterClient::DoConnect()
    {
        if (!_ioPool)
        {
            return;
        }

        auto &ctx = _ioPool->GetNextContext();

        asio::ip::tcp::resolver resolver(ctx);
        asio::error_code        resolveEc;
        auto                    endpoints = resolver.resolve(_host, std::to_string(_port), resolveEc);
        if (resolveEc || endpoints.empty())
        {
            Log::Error("CenterClient: resolve {}:{} failed: {}, retrying in {}ms",
                       _host,
                       _port,
                       resolveEc.message(),
                       kRetryIntervalMs);
            ScheduleConnectRetry();
            return;
        }

        _socket = std::make_shared<TCPSocket>(asio::ip::tcp::socket(ctx), EFraming::LengthPrefix);
        _socket->SetBackPressure(EBackPressure::DropOldest);

        _socket->SetMessageHandler(
            [this](uint32 /*msgID*/, uint32 /*sessionID*/, const uint8 *body, size_t len) {
                // LengthPrefix 模式下收到的是 RPC 帧 → 解析 RPCHeader 路由
                if (len < sizeof(RPCHeader))
                {
                    return;
                }
                auto   headerBuf = ByteBuffer::Wrap(body, sizeof(RPCHeader));
                uint32 rpcMsgID  = headerBuf.ReadUint32();
                uint64 requestID = headerBuf.ReadUint64();
                /*traceID=*/headerBuf.ReadUint64();
                /*type=*/headerBuf.ReadUint8();

                size_t       protoLen  = len - sizeof(RPCHeader);
                const uint8 *protoData = body + sizeof(RPCHeader);

                if (rpcMsgID == Proto::Internal::MSG_REGISTER_WORLD_RSP)
                {
                    Proto::Internal::RegisterWorldRsp rsp;
                    if (rsp.ParseFromArray(protoData, static_cast<int>(protoLen)) && rsp.ok())
                    {
                        _connected.store(true, std::memory_order_release);
                        Log::Info("CenterClient: registered as worldServerID={}", _worldServerID);
                        // 注册成功后批量 dump 在线玩家，重建 Center 索引
                        SendBatchOnlinePlayers();
                    }
                }
                else
                {
                    // 其他 RPC 响应 → 交给 RPCClient 按 requestID 匹配回调
                    _rpcClient.OnResponse(requestID, protoData, protoLen);
                }
            });

        _socket->LowestLayer().async_connect(*endpoints.begin(), [this](const asio::error_code &ec) {
            if (ec)
            {
                Log::Error("CenterClient: connect to {}:{} failed: {}, retrying in {}ms",
                           _host,
                           _port,
                           ec.message(),
                           kRetryIntervalMs);
                _socket.reset();
                _connected.store(false, std::memory_order_release);
                ScheduleConnectRetry();
                return;
            }

            Log::Info("CenterClient: connected to {}:{}", _host, _port);

            // 连接成功后启动异步读循环 + 发送 RegisterWorldReq
            _socket->Start();
            SendRegisterWorld();
        });
    }

    void CenterClient::ScheduleConnectRetry()
    {
        if (!_ioPool)
        {
            return;
        }

        auto &ctx   = _ioPool->GetNextContext();
        auto  timer = std::make_shared<asio::steady_timer>(ctx, std::chrono::milliseconds(kRetryIntervalMs));
        timer->async_wait([this, timer](const asio::error_code &ec) {
            if (ec)
            {
                return;
            }
            DoConnect();
        });
    }

    void CenterClient::SendRegisterWorld()
    {
        Proto::Internal::RegisterWorldReq req;
        req.set_service_id(std::to_string(_worldServerID));
        req.set_address(_address);
        req.set_max_players(_maxPlayers);

        auto frame = BuildRPCFrame(Proto::Internal::MSG_REGISTER_WORLD_REQ, 0, ERPCType::Request, 0, req);
        if (frame.Size() > 0)
        {
            _socket->Send(std::move(frame));
        }
    }

    void CenterClient::SendBatchOnlinePlayers()
    {
        if (!_collector)
        {
            return; // 未设置收集器（WorldServer 尚未初始化完）
        }

        auto ids = _collector();
        if (ids.empty())
        {
            return;
        }

        Proto::Internal::BatchOnlinePlayersNtf ntf;
        ntf.set_world_server_id(_worldServerID);
        for (uint32 accountID : ids)
        {
            ntf.add_account_ids(accountID);
        }
        _rpcClient.Notify(_socket, Proto::Internal::MSG_BATCH_PLAYERS_ONLINE_NTF, ntf);
        Log::Info("CenterClient: replayed {} online players to Center", ids.size());
    }

    void CenterClient::SendHeartbeat(uint32 currentPlayers)
    {
        if (!_connected.load(std::memory_order_acquire))
        {
            return;
        }

        Proto::Internal::HeartbeatReq req;
        req.set_current_players(currentPlayers);
        _rpcClient.Notify(_socket, Proto::Internal::MSG_HEARTBEAT_REQ, req);
    }

} // namespace MMO
