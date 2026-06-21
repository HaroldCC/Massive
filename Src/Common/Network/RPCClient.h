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
#include <string>
#include <unordered_map>
#include <vector>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"
#include "Common/Network/RPCHeader.h"
#include "Common/Network/TCPSocket.h"
#include "Common/Queue/MPSCQueue.h"

namespace MMO
{

class TCPSocket;

// —————— 辅助：将 RPCHeader + protobuf body 序列化为 ByteBuffer ——————

/**
 * @brief 构造完整的 RPC 帧（RPCHeader + protobuf body）
 * @param msgID     EInternalMsgID
 * @param requestID requestID
 * @param type      Request / Response / Notify
 * @param traceID   链路追踪 ID
 * @param body      protobuf 序列化字符串（move）
 * @return Own 模式的 ByteBuffer
 */
ByteBuffer BuildRPCFrame(uint32 msgID, uint64 requestID, ERPCType type,
                         uint64 traceID, std::string&& body);

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
    void Call(std::shared_ptr<TCPSocket> conn,
              uint32 msgID,
              const TReq& req,
              std::function<void(const TRsp&)> onReply,
              std::function<void()> onError = nullptr,
              std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    /**
     * @brief 单向通知（无回包）
     */
    template <typename TMsg>
    void Notify(std::shared_ptr<TCPSocket> conn, uint32 msgID, const TMsg& msg);

    // 收到 RPC 响应（IO 线程调用）
    void OnResponse(uint64 requestID, const uint8* body, size_t len);

    // 连接断开 → 所有在途请求触发 onError
    void OnConnectionLost();

    // 每 Tick 检查超时（LogicThread 调用）
    void ProcessTimeouts();

    // 当前在途请求数
    size_t PendingCount() const { return _pending.size(); }

private:
    struct PendingCall
    {
        std::function<void(const uint8*, size_t)> deserializeAndCallback;
        std::function<void()>                     onError;
        std::chrono::steady_clock::time_point     deadline;
    };

    void EnqueueCallback(std::function<void()> fn);

    std::unordered_map<uint64, PendingCall> _pending;
    std::atomic<uint64> _nextRequestID{1};
    bool _useLogicThread;
    MPSCQueue<Completed> _completedQueue;
};

// ── 模板实现 ──

template <typename TReq, typename TRsp>
void RPCClient::Call(std::shared_ptr<TCPSocket> conn,
                     uint32 msgID,
                     const TReq& req,
                     std::function<void(const TRsp&)> onReply,
                     std::function<void()> onError,
                     std::chrono::milliseconds timeout)
{
    uint64 id = _nextRequestID.fetch_add(1, std::memory_order_relaxed);

    auto deserializeAndCallback = [onReply = std::move(onReply)](const uint8* body, size_t len)
    {
        TRsp rsp;
        if (!rsp.ParseFromArray(body, static_cast<int>(len)))
        {
            return;
        }
        onReply(rsp);
    };

    _pending[id] = PendingCall{
        std::move(deserializeAndCallback),
        std::move(onError),
        std::chrono::steady_clock::now() + timeout
    };

    auto body = req.SerializeAsString();
    auto frame = BuildRPCFrame(msgID, id, ERPCType::Request, 0, std::move(body));
    conn->Send(std::move(frame));
}

template <typename TMsg>
void RPCClient::Notify(std::shared_ptr<TCPSocket> conn, uint32 msgID, const TMsg& msg)
{
    auto body = msg.SerializeAsString();
    auto frame = BuildRPCFrame(msgID, 0, ERPCType::Notify, 0, std::move(body));
    conn->Send(std::move(frame));
}

} // namespace MMO
