/**
 * @file GateConnection.cpp
 * @brief GateConnectionMgr 实现
 */

#include "World/GateConnection.h"
#include "Common/Log/Log.h"
#include "Common/Network/PacketHeader.h"

#include <MsgID.pb.h>

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
        uint32 msgID = ByteBuffer::Wrap(data, sizeof(uint32)).ReadUint32();

        {
            std::shared_lock lock(*_sessionsMtx);
            auto it = _sessions->find(sessionID);
            if (it != _sessions->end())
            {
                auto &ws = it->second;

                // 入站帧: [PacketHeader:12B][Seq:4B][Ciphertext+Tag]
                if (len <= sizeof(PacketHeader) + sizeof(uint32))
                {
                    Log::Warn("GateConnection: undersized business msg session={} msgID={}", sessionID, msgID);
                    return;
                }

                // 提取 Seq
                uint32     seq     = ByteBuffer::Wrap(data + sizeof(PacketHeader), sizeof(uint32)).ReadUint32();
                size_t     encLen  = len - sizeof(PacketHeader) - sizeof(uint32);
                const uint8 *encBody = data + sizeof(PacketHeader) + sizeof(uint32);

                // AES-GCM 解密
                auto plaintext = ws.crypto.Decrypt(encBody, encLen, seq);
                if (!plaintext)
                {
                    Log::Debug("GateConnection: decrypt failed session={} msgID={}", sessionID, msgID);
                    return;
                }

                LogicMessage logicMsg;
                logicMsg.sessionID = sessionID;
                logicMsg.msgID     = msgID;
                logicMsg.body      = std::move(*plaintext);
                logicMsg.recvTime  = std::chrono::steady_clock::now();

                ws.inbox.Enqueue(std::move(logicMsg));
                return;
            }
        }

        // session 未找到——可能是首条 EnterWorldReq，走 Fallback 队列
        if (msgID == Proto::MSG_LOGIN_ENTER_WORLD_REQ)
        {
            // 直接 Enqueue 到一个特殊队列，LogicThread 在 ProcessMessages 额外处理
            LogicMessage logicMsg;
            logicMsg.sessionID = sessionID;
            logicMsg.msgID     = msgID;
            logicMsg.body      = ByteBuffer::Copy(data, len);
            logicMsg.recvTime  = std::chrono::steady_clock::now();
            _unroutedQueue.Enqueue(std::move(logicMsg));
        }
        else
        {
            Log::Debug("GateConnection: no session {} for msgID={}", sessionID, msgID);
        }
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
