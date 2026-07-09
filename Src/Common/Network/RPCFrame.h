/**
 * @file RPCFrame.h
 * @brief RPC 帧构建辅助——零分配序列化 RPCHeader + protobuf body 到 ByteBuffer
 *
 * 核心函数: BuildRPCFrame<TMsg>(msgID, requestID, type, traceID, msg)
 *
 * 序列化优化: 使用 ByteSizeLong() 预计算 + SerializeToArray() 直接写入，
 * 零中间 string 分配。RPCClient::Call/Notify 和 RPCContext::Reply 均使用本函数。
 */
#pragma once

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/MassiveAssert.h"
#include "Common/Core/Types.h"
#include "Common/Network/RPCHeader.h"

namespace MMO
{

    /**
     * @brief 构造完整 RPC 帧（LengthPrefix + RPCHeader + protobuf body），零中间 string 分配
     *
     * 输出格式（LengthPrefix 帧协议）：
     *   [totalLen:4B] [RPCHeader:17B] [protobuf body]
     * totalLen 包含自身 4B + RPCHeader + body，与 TCPSocket::ProcessLengthPrefixed 兼容。
     *
     * @tparam TMsg  protobuf message 类型（Req/Rsp/Ntf）
     * @param msgID     EInternalMsgID
     * @param requestID requestID（0 = Notify）
     * @param type      Request / Response / Notify
     * @param traceID   链路追踪 ID
     * @param msg       protobuf message 引用
     * @return Own 模式的 ByteBuffer（LengthPrefix + RPCHeader + protobuf body）
     */
    template <typename TMsg>
    ByteBuffer BuildRPCFrame(uint32 msgID, uint64 requestID, ERPCType type, uint64 traceID, const TMsg &msg)
    {
        size_t bodySize  = static_cast<size_t>(msg.ByteSizeLong());
        size_t headerLen = sizeof(RPCHeader);
        size_t total     = sizeof(uint32) + headerLen + bodySize;
        auto   buf       = ByteBuffer::Own(total);

        // 写 LengthPrefix（大端总长度，含自身 4B）
        buf.WriteUint32(static_cast<uint32>(total));

        // 写 RPCHeader
        buf.WriteUint32(msgID);
        buf.WriteUint64(requestID);
        buf.WriteUint64(traceID);
        buf.WriteUint8(static_cast<uint8>(type));

        // 写 protobuf body（零 copy 写入）
        bool ok = msg.SerializeToArray(buf.WritePtr(), static_cast<int>(bodySize));
        MASSIVE_ASSERT(ok, "SerializeToArray failed -- message may be incomplete");
        if (!ok)
        {
            return ByteBuffer {};
        }

        buf.SetWritePos(buf.WritePos() + bodySize);

        return buf;
    }

} // namespace MMO
