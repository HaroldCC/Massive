/**
 * @file MessageDispatcher.h
 * @brief MsgID 函数表分发器
 *
 * std::array<Handler, kMaxHandlers> 做 O(1) 查表，msgID 直接作为数组下标。
 * Register<T> 自动完成 protobuf 解析，handler 收到的是强类型消息对象。
 *
 * 使用示例：
 * @code
 *   MessageDispatcher<Entity> dispatcher;
 *   dispatcher.Register<MoveReq>(MSG_MOVE_REQ, [](Entity e, const MoveReq& msg) {
 *       HandleMove(e, msg.x(), msg.y());
 *   });
 *   dispatcher.Dispatch(entity, msgID, body, len);
 * @endcode
 */
#pragma once

#include <array>
#include <functional>

#include "Common/Core/Types.h"
#include "Common/Log/Log.h"

namespace MMO
{

    /**
     * @brief MsgID 函数表分发器
     *
     * @tparam TContext  分发上下文类型（Gate 用 sessionID，World 用 Entity）
     */
    template <typename TContext>
    class MessageDispatcher
    {
    public:
        using Handler = std::function<void(TContext, const uint8 *, size_t)>;

        /**
         * @brief 注册消息处理器
         * @tparam MsgID 消息 ID（编译期常量，static_assert 确保不越界）
         * @tparam TMsg  protobuf 消息类型
         * @param handler  强类型处理回调 void(TContext, const TMsg&)
         */
        template <uint32 MsgID, typename TMsg>
        void Register(std::function<void(TContext, const TMsg &)> handler)
        {
            static_assert(
                MsgID < kMaxHandlers,
                "MsgID exceeds kMaxHandlers -- increase kMaxHandlers in Types.h or fix the MsgID enum");

            if (_handlers[MsgID])
            {
                Log::Warn("MessageDispatcher: msgID {} already registered, overwriting", MsgID);
            }

            _handlers[MsgID] = [handler = std::move(handler)](TContext ctx, const uint8 *body, size_t len) {
                TMsg msg;
                if (!msg.ParseFromArray(body, static_cast<int>(len)))
                {
                    Log::Error("MessageDispatcher: protobuf parse failed ({} bytes)", len);
                    return;
                }
                handler(ctx, msg);
            };
        }

        /**
         * @brief 分发消息到对应处理器
         * @param ctx    分发上下文
         * @param msgID  消息 ID
         * @param body   protobuf 序列化数据
         * @param len    数据长度
         * @return 找到并分发返回 true；无对应处理器返回 false
         */
        bool Dispatch(TContext ctx, uint32 msgID, const uint8 *body, size_t len) const
        {
            if (msgID >= kMaxHandlers || !_handlers[msgID])
            {
                return false;
            }
            _handlers[msgID](ctx, body, len);
            return true;
        }

        // 是否已注册某 msgID
        bool IsRegistered(uint32 msgID) const
        {
            return msgID < kMaxHandlers && static_cast<bool>(_handlers[msgID]);
        }

    private:
        std::array<Handler, kMaxHandlers> _handlers;
    };

} // namespace MMO
