/**
 * @file RPCContext.h
 * @brief RPC handler 上下文：requestID + socket 引用
 *
 * handler 自行用 BuildRPCFrame 构造回包：
 * @code
 *   auto body = rsp.SerializeAsString();
 *   auto frame = BuildRPCFrame(msgID, ctx.requestID, ERPCType::Response, 0, std::move(body));
 *   ctx.socket->Send(std::move(frame));
 * @endcode
 */
#pragma once

#include <memory>

#include "Common/Core/Types.h"

namespace MMO
{

class TCPSocket;

struct RPCContext
{
    uint64 requestID = 0;
    std::shared_ptr<TCPSocket> socket;
};

} // namespace MMO
