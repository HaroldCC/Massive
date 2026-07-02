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
        auto conn    = std::make_unique<GateConnection>();
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

                ByteBuffer headerBuf       = ByteBuffer::Wrap(data, sizeof(uint32));
                uint32     packetSessionID = headerBuf.ReadUint32();

                OnGateMessage(packetSessionID, data + sizeof(uint32), len - sizeof(uint32));
            });

        // 必须在 move<unique_ptr> 之前 Start，否则 conn 为空指针
        conn->socket->Start();

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

        // data = [PacketHeader:12B][Body]，PacketHeader = {length, msgID, sessionID}
        // 控制消息（sessionID == 0）：DisconnectNtf 等
        // 控制消息不经过 PacketHeader 包装——body 直接以 [ctrlMsgID:4B][ctrlBody] 格式传输
        if (sessionID == 0 && len >= sizeof(uint32))
        {
            uint32 ctrlMsgID = ByteBuffer::Wrap(data, sizeof(uint32)).ReadUint32();
            HandleControlMessage(ctrlMsgID, data + sizeof(uint32), len - sizeof(uint32));
            return;
        }

        // 业务消息：从 PacketHeader.msgID（偏移 4）读取
        if (len < sizeof(PacketHeader))
        {
            Log::Warn("GateConnection: undersized frame ({} bytes)", len);
            return;
        }
        uint32 msgID = ByteBuffer::Wrap(data + sizeof(uint32), sizeof(uint32)).ReadUint32();

        {
            std::shared_lock lock(*_sessionsMtx);
            auto             it = _sessions->find(sessionID);
            if (it != _sessions->end())
            {
                auto &ws = it->second;

                // 入站帧: [PacketHeader:12B][Seq:4B][Ciphertext+Tag]
                if (len <= sizeof(PacketHeader) + sizeof(uint32))
                {
                    Log::Warn("GateConnection: undersized business msg session={} msgID={}",
                              sessionID,
                              msgID);
                    return;
                }

                // 提取 Seq
                uint32       seq = ByteBuffer::Wrap(data + sizeof(PacketHeader), sizeof(uint32)).ReadUint32();
                size_t       encLen  = len - sizeof(PacketHeader) - sizeof(uint32);
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
            // data = [PacketHeader:12B][Body]，只拷贝 Body（protobuf）
            size_t       bodyLen = len - sizeof(PacketHeader);
            LogicMessage logicMsg;
            logicMsg.sessionID = sessionID;
            logicMsg.msgID     = msgID;
            logicMsg.body      = ByteBuffer::Copy(data + sizeof(PacketHeader), bodyLen);
            logicMsg.recvTime  = std::chrono::steady_clock::now();
            _unroutedQueue.Enqueue(std::move(logicMsg));
        }
        else
        {
            Log::Debug("GateConnection: no session {} for msgID={}", sessionID, msgID);
        }
    }

    void GateConnectionMgr::HandleControlMessage(uint32 ctrlMsgID, const uint8 *data, size_t len)
    {
        // Enqueue 到 _ctrlQueue，由 LogicThread 在 OnTick 中消费
        LogicMessage msg;
        msg.sessionID = 0;
        msg.msgID     = ctrlMsgID;
        msg.body      = ByteBuffer::Copy(data, len);
        msg.recvTime  = std::chrono::steady_clock::now();
        _ctrlQueue.Enqueue(std::move(msg));
    }

    void GateConnectionMgr::SendToGate(uint16 gateID, uint32 sessionID, ByteBuffer payload)
    {
        // 构建 Gate 帧：[LengthPrefix:4B][InternalHeader:4B=sessionID][payload]
        // Gate↔World 连接使用 Framing::LengthPrefix，Gate 侧 ProcessLengthPrefixed
        // 先读 4B totalLen，再收齐 totalLen 字节。totalLen 包含后续 InternalHeader + payload。
        uint32     internalLen = static_cast<uint32>(sizeof(uint32) + payload.Size());
        uint32     totalLen    = sizeof(uint32) + internalLen;
        ByteBuffer frame       = ByteBuffer::Own(totalLen);
        frame.WriteUint32(totalLen);  // LengthPrefix（不含自身）
        frame.WriteUint32(sessionID); // InternalHeader.sessionID
        frame.WriteBytes(payload.Data(), payload.Size());

        {
            std::lock_guard lock(_gateMutex);
            auto            it = _gateConns.find(gateID);
            if (it == _gateConns.end())
            {
                Log::Warn("GateConnection: gateID={} not connected", gateID);
                return;
            }
            it->second->socket->Send(std::move(frame));
        }
    }

} // namespace MMO
