/**
 * @file RPCContext.h
 * @brief RPC handler 上下文：requestID + msgID + socket 引用 + Reply 便捷方法
 *
 * 序列化优化：Reply 使用 ByteSizeLong() + SerializeToArray() 直接写入 ByteBuffer，
 * 零中间 string 分配。
 *
 * 用法：
 * @code
 *   _rpcHandlers.Register<QueryPlayerLocationReq>(
 *       MSG_QUERY_PLAYER_LOCATION_REQ,
 *       [this](RPCContext ctx, const QueryPlayerLocationReq& req) {
 *           QueryPlayerLocationRsp rsp;
 *           rsp.set_world_server_id("...");
 *           ctx.Reply(rsp);  // 零拷贝序列化 + Send
 *       });
 * @endcode
 */
#pragma once

#include <memory>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"
#include "Common/Network/RPCFrame.h"
#include "Common/Network/RPCHeader.h"
#include "Common/Network/TCPSocket.h"

namespace MMO
{

    struct RPCContext
    {
        uint64                     requestID = 0; ///< RPC 请求 ID（由发起方分配）
        uint32                     msgID     = 0; ///< EInternalMsgID（用于构建响应帧头）
        std::shared_ptr<TCPSocket> socket;

        /**
         * @brief 回包：ByteSizeLong() 预计算 → SerializeToArray() 零拷⻉ → Send
         *
         * 可在 IO 线程或 LogicThread 中调用，Send 内部保证线程安全。
         */
        template <typename TMsg>
        void Reply(const TMsg &msg) const
        {
            auto frame = BuildRPCFrame(msgID, requestID, ERPCType::Response, 0, msg);
            if (frame.Size() == 0)
            {
                return; // 序列化失败，不发送
            }
            socket->Send(std::move(frame));
        }
    };

} // namespace MMO
