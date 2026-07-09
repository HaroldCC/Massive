/**
 * @file RPCClient.h
 * @brief RPC 发起方——Call/Notify + requestID 匹配 + 超时/断线保障
 *
 * 核心保证："一定有结果"——成功/超时/断线三选一回调，绝不永久挂起。
 */
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <asio/steady_timer.hpp>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"
#include "Common/Network/RPCFrame.h"
#include "Common/Network/RPCHeader.h"
#include "Common/Network/TCPSocket.h"
#include "Common/Queue/MPSCQueue.h"

namespace MMO
{

    class TCPSocket;

    /**
     * @brief RPC 发起方
     *
     * 回调可投递到 LogicThread（MPSCQueue）或 IO 线程直跑。
     */
    class RPCClient
    {
    public:
        struct Completed
        {
            std::function<void()> fn;
        };

        // useLogicThread: true → 回调投递到 LogicThread（MPSCQueue，World 侧）
        //                false → 回调在 IO 线程直接执行（Center/Social 侧）
        explicit RPCClient(bool useLogicThread = true);

        /**
         * @brief 发起 RPC
         * @tparam TReq   请求 protobuf 类型
         * @tparam TRsp   响应 protobuf 类型
         * @param conn      TCPSocket 长连接
         * @param msgID     内部消息 ID（EInternalMsgID）
         * @param req       请求消息
         * @param onReply   成功回调
         * @param onError   失败回调（超时/断线/序列化失败，可选）
         * @param timeout   超时时间（默认 5s）
         */
        template <typename TReq, typename TRsp>
        void Call(std::shared_ptr<TCPSocket>        conn,
                  uint32                            msgID,
                  const TReq                       &req,
                  std::function<void(const TRsp &)> onReply,
                  std::function<void()>             onError = nullptr,
                  std::chrono::milliseconds         timeout = std::chrono::milliseconds(5000));

        /**
         * @brief 单向通知（无回包）
         */
        template <typename TMsg>
        void Notify(std::shared_ptr<TCPSocket> conn, uint32 msgID, const TMsg &msg);

        // 收到 RPC 响应（IO 线程调用）
        void OnResponse(uint64 requestID, const uint8 *body, size_t len);

        // 连接断开 → 所有在途请求触发 onError
        void OnConnectionLost();

        // 每 Tick 检查超时（LogicThread 调用）
        void ProcessTimeouts();

        /**
         * @brief 在 IO 线程启动独立定时器做超时兜底
         *        首次由 CenterClient 连接建立后调用
         * @param ioCtx  IOContextPool 的某个 io_context
         */
        void StartTimeoutChecker(asio::io_context &ioCtx);

        // 当前在途请求数
        size_t PendingCount() const
        {
            return _pending.size();
        }

        /**
         * @brief 排干已完成回调队列（LogicThread 每 Tick 调用）
         *        配合 _useLogicThread=true 模式，确保 RPC 响应/超时被消费
         */
        void DrainCompleted();

    private:
        struct PendingCall
        {
            std::function<void(const uint8 *, size_t)> deserializeAndCallback;
            std::function<void()>                      onError;
            std::chrono::steady_clock::time_point      deadline;
        };

        void EnqueueCallback(std::function<void()> fn);
        void ScheduleTimeoutCheck();

        std::unordered_map<uint64, PendingCall> _pending;
        std::mutex                              _pendingMtx; // LogicThread 写 / IO 线程读
        std::atomic<uint64>                     _nextRequestID {1};
        bool                                    _useLogicThread;
        MPSCQueue<Completed>                    _completedQueue;

        // IO 线程独立超时检查定时器
        std::unique_ptr<asio::steady_timer> _timeoutTimer;
        bool                                _timerStarted = false;
    };

    // ── 模板实现 ──

    template <typename TReq, typename TRsp>
    void RPCClient::Call(std::shared_ptr<TCPSocket>        conn,
                         uint32                            msgID,
                         const TReq                       &req,
                         std::function<void(const TRsp &)> onReply,
                         std::function<void()>             onError,
                         std::chrono::milliseconds         timeout)
    {
        uint64 id = _nextRequestID.fetch_add(1, std::memory_order_relaxed);

        auto deserializeAndCallback = [onReply = std::move(onReply)](const uint8 *body, size_t len) {
            TRsp rsp;
            if (!rsp.ParseFromArray(body, static_cast<int>(len)))
            {
                return;
            }
            onReply(rsp);
        };

        _pending[id] = PendingCall {std::move(deserializeAndCallback),
                                    std::move(onError),
                                    std::chrono::steady_clock::now() + timeout};

        auto frame = BuildRPCFrame(msgID, id, ERPCType::Request, 0, req);
        if (frame.Size() == 0)
        {
            _pending.erase(id); // 序列化失败: 不发送，不留下悬挂 pending
            if (onError)
            {
                onError(); // 立刻通知调用方失败
            }
            return;
        }
        conn->Send(std::move(frame));
    }

    template <typename TMsg>
    void RPCClient::Notify(std::shared_ptr<TCPSocket> conn, uint32 msgID, const TMsg &msg)
    {
        auto frame = BuildRPCFrame(msgID, 0, ERPCType::Notify, 0, msg);
        if (frame.Size() == 0)
        {
            return; // 序列化失败，不发送
        }
        conn->Send(std::move(frame));
    }

} // namespace MMO
