/**
 * @file GateConnection.cpp
 * @brief GateConnectionMgr 实现
 */

#include "World/GateConnection.h"
#include "Common/Log/Log.h"
#include "Common/Network/PacketHeader.h"

namespace MMO
{

    void GateConnectionMgr::AcceptConnection(uint16 gateID, std::shared_ptr<TCPSocket> socket)
    {
        auto conn = std::make_unique<GateConnection>();
        conn->gateID = gateID;
        conn->socket = std::move(socket);

        // 注册消息回调（IO 线程回调，拆 InternalHeader 后写入对应 Session inbox）
        auto weakSock = std::weak_ptr<TCPSocket>(conn->socket);
        conn->socket->SetMessageHandler(
            [this, gateID](uint32 /*msgID*/, uint32 /*sessionID*/, const uint8 *data, size_t len) {
                // data = [InternalHeader:4B][payload]
                if (len < sizeof(uint32))
                {
                    Log::Warn("GateConnection: undersized frame ({} bytes)", len);
                    return;
                }

                ByteBuffer headerBuf = ByteBuffer::Wrap(data, sizeof(uint32));
                uint32     packetSessionID = headerBuf.ReadUint32();

                OnGateMessage(packetSessionID, data + sizeof(uint32), len - sizeof(uint32));
            });

        {
            std::lock_guard lock(_gateMutex);
            _gateConns[gateID] = std::move(conn);
        }

        Log::Info("GateConnection: gateID={} connected", gateID);
    }

    void GateConnectionMgr::OnGateMessage(uint32 sessionID, const uint8 *data, size_t len)
    {
        if (!_sessionsMtx || !_sessions)
        {
            return;
        }

        // IO 线程：读锁查找 session → Enqueue 到其 inbox
        std::shared_lock lock(*_sessionsMtx);
        auto it = _sessions->find(sessionID);
        if (it == _sessions->end())
        {
            Log::Debug("GateConnection: no session {} (disconnected)", sessionID);
            return;
        }

        auto &ws = it->second;

        // 构造 LogicMessage
        LogicMessage msg;
        msg.sessionID = sessionID;
        msg.msgID     = ByteBuffer::Wrap(data, sizeof(uint32)).ReadUint32();
        msg.body      = ByteBuffer::Copy(data, len);
        msg.recvTime  = std::chrono::steady_clock::now();

        // Enqueue 到 Per-Session inbox（无锁，MPSCQueue 多生产者单消费者安全）
        ws.inbox.Enqueue(std::move(msg));
    }

    void GateConnectionMgr::SendToGate(uint16 gateID, uint32 sessionID, ByteBuffer payload)
    {
        // 构建 Gate 帧：[InternalHeader:4B(sessionID)][payload]
        size_t  totalSize = sizeof(uint32) + payload.Size();
        ByteBuffer frame = ByteBuffer::Own(totalSize);
        frame.WriteUint32(sessionID);
        frame.WriteBytes(payload.Data(), payload.Size());

        {
            std::lock_guard lock(_gateMutex);
            auto it = _gateConns.find(gateID);
            if (it == _gateConns.end())
            {
                Log::Warn("GateConnection: gateID={} not connected", gateID);
                return;
            }
            it->second->socket->Send(std::move(frame));
        }
    }

} // namespace MMO
