/**
 * @file RPCHeader.h
 * @brief 内部 RPC 帧头（大端序）
 *
 * Wire 上 RPCHeader(15B) + protobuf body。
 * type 区分 Request/Response/Notify，requestID 做请求-响应关联。
 */
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

// RPC 消息类型
enum class ERPCType : uint8
{
    Request  = 0,  // 请求（需回包）
    Response = 1,  // 响应（回填 requestID）
    Notify   = 2,  // 单向通知（无需回包）
};

#pragma pack(push, 1)
/**
 * @brief 内部 RPC 帧头（15 字节）
 */
struct RPCHeader
{
    uint32 msgID;     // EInternalMsgID，大端
    uint64 requestID; // 请求-响应关联：Response 原样回填
    uint64 traceID;   // 链路追踪 ID
    uint8  type;      // ERPCType: 0=Request, 1=Response, 2=Notify
};
#pragma pack(pop)

} // namespace MMO
