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

        // 建立 TCP 连接到 CenterServer（LengthPrefix 帧协议）
        auto &ctx = ioPool->GetNextContext();

        asio::ip::tcp::resolver resolver(ctx);
        asio::error_code        resolveEc;
        auto                    endpoints = resolver.resolve(host, std::to_string(port), resolveEc);
        if (resolveEc || endpoints.empty())
        {
            Log::Error("CenterClient: resolve {}:{} failed: {}", host, port, resolveEc.message());
            return false;
        }

        _socket = std::make_shared<TCPSocket>(asio::ip::tcp::socket(ctx), Framing::LengthPrefix);
        bool connected = false;
        asio::error_code connectEc;

        // 同步连接（Init 阶段，阻塞可接受）
        _socket->LowestLayer().connect(*endpoints.begin(), connectEc);
        if (connectEc)
        {
            Log::Error("CenterClient: connect to {}:{} failed: {}", host, port, connectEc.message());
            _socket.reset();
            return false;
        }

        // 注册回调
        _socket->SetMessageHandler([this](uint32 /*msgID*/, uint32 /*sessionID*/,
                                          const uint8 *body, size_t len) {
            // LengthPrefix 模式下收到的是 RPC 帧 → 解析 RPCHeader 路由
            if (len < sizeof(RPCHeader))
            {
                return;
            }
            auto   headerBuf = ByteBuffer::Wrap(body, sizeof(RPCHeader));
            uint32 rpcMsgID  = headerBuf.ReadUint32();
            uint64 requestID = headerBuf.ReadUint64();
            /*traceID=*/headerBuf.ReadUint64();
            /*type=*/  headerBuf.ReadUint8();

            size_t       protoLen  = len - sizeof(RPCHeader);
            const uint8 *protoData = body + sizeof(RPCHeader);

            if (rpcMsgID == Proto::Internal::MSG_REGISTER_WORLD_RSP)
            {
                Proto::Internal::RegisterWorldRsp rsp;
                if (rsp.ParseFromArray(protoData, static_cast<int>(protoLen)) && rsp.ok())
                {
                    _connected.store(true, std::memory_order_release);
                    Log::Info("CenterClient: registered as worldServerID={}", _worldServerID);
                }
            }
            else
            {
                // 其他 RPC 响应 → 交给 RPCClient 按 requestID 匹配回调
                _rpcClient.OnResponse(requestID, protoData, protoLen);
            }
        });

        _socket->Start();

        // 发送 RegisterWorldReq
        Proto::Internal::RegisterWorldReq req;
        req.set_service_id(std::to_string(worldServerID));
        req.set_address(address);
        req.set_max_players(maxPlayers);

        auto frame = BuildRPCFrame(
            Proto::Internal::MSG_REGISTER_WORLD_REQ, 0, ERPCType::Request, 0, req);
        if (frame.Size() > 0)
        {
            _socket->Send(std::move(frame));
        }

        return true;
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
