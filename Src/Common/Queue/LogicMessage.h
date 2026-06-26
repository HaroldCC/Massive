/**
 * @file LogicMessage.h
 * @brief 网络线程 → 逻辑线程 消息载体
 */
#pragma once

#include <chrono>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 网络线程到逻辑线程的消息载体
     *
     * 网络线程收到完整包后，构造 LogicMessage 通过 MPSCQueue 投递到逻辑线程。
     */
    struct LogicMessage
    {
        uint32                                sessionID     = 0; // 来源连接 ID
        uint32                                msgID         = 0; // 消息协议 ID
        uint64                                traceID       = 0; // 链路追踪 ID
        uint64                                clientTraceID = 0; // 客户端链路追踪 ID
        ByteBuffer                            body;              // 消息体（Wrap 模式，零 copy）
        std::chrono::steady_clock::time_point recvTime;          // 接收时间戳
    };

} // namespace MMO
