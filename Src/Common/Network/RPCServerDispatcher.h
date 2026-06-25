/**
 * @file RPCServerDispatcher.h
 * @brief RPC handler 注册便捷别名
 *
 * RPCServerDispatcher 是 MessageDispatcher<RPCContext> 的类型别名，
 * 专用于 Center/Social 等纯 IO 进程的 RPC handler 注册。
 *
 * 使用示例：
 * @code
 *   RPCServerDispatcher _rpcHandlers;
 *   _rpcHandlers.Register<RegisterWorldReq>(
 *       MSG_REGISTER_WORLD_REQ,
 *       [this](RPCContext ctx, const RegisterWorldReq& req) {
 *           RegisterWorldRsp rsp;
 *           _services.Register({...});
 *           ctx.Reply(rsp);
 *       });
 * @endcode
 */
#pragma once

#include "Common/Network/MessageDispatcher.h"
#include "Common/Network/RPCContext.h"

namespace MMO
{

    /** @brief RPC handler 分发器（Center/Social 侧） */
    using RPCServerDispatcher = MessageDispatcher<RPCContext>;

} // namespace MMO
