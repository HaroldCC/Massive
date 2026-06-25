/**
 * @file RPCClient.cpp
 * @brief RPCClient 非模板实现
 */

#include "Common/Network/RPCClient.h"
#include "Common/Network/TCPSocket.h"
#include "Common/Log/Log.h"

namespace MMO
{

// ===== RPCClient =====

RPCClient::RPCClient(bool useLogicThread)
    : _useLogicThread(useLogicThread)
{
}

void RPCClient::OnResponse(uint64 requestID, const uint8* body, size_t len)
{
    auto it = _pending.find(requestID);
    if (it == _pending.end())
    {
        return;  // 已超时/断线清理，丢弃
    }

    if (it->second.deserializeAndCallback)
    {
        auto cb = std::move(it->second.deserializeAndCallback);
        // 无论哪种线程模型，反序列化 + 回调都投递到目标线程
        EnqueueCallback([cb = std::move(cb), body = std::vector<uint8>(body, body + len)]()
        {
            cb(body.data(), body.size());
        });
    }
    _pending.erase(it);
}

void RPCClient::OnConnectionLost()
{
    for (auto& [id, call] : _pending)
    {
        if (call.onError)
        {
            EnqueueCallback([cb = std::move(call.onError)]() { cb(); });
        }
    }
    _pending.clear();
}

void RPCClient::ProcessTimeouts()
{
    auto now = std::chrono::steady_clock::now();
    for (auto it = _pending.begin(); it != _pending.end(); )
    {
        if (now > it->second.deadline)
        {
            if (it->second.onError)
            {
                EnqueueCallback([cb = std::move(it->second.onError)]() { cb(); });
            }
            it = _pending.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void RPCClient::EnqueueCallback(std::function<void()> fn)
{
    if (_useLogicThread)
    {
        // 投递到 MPSCQueue，等 LogicThread::ProcessRPCResponses() 消费
        _completedQueue.Enqueue(Completed{std::move(fn)});
    }
    else
    {
        // IO 线程直跑（Center/Social 侧）
        fn();
    }
}

} // namespace MMO
