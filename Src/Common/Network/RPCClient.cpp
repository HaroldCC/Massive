/**
 * @file RPCClient.cpp
 * @brief RPCClient 非模板实现
 */

#include "Common/Network/RPCClient.h"
#include "Common/Network/TCPSocket.h"
#include "Common/Log/Log.h"

#include <asio/post.hpp>

namespace MMO
{

    // ── RPCClient ──

    RPCClient::RPCClient(bool useLogicThread) : _useLogicThread(useLogicThread)
    {
    }

    void RPCClient::OnResponse(uint64 requestID, const uint8 *body, size_t len)
    {
        PendingCall call;
        {
            std::lock_guard lock(_pendingMtx);
            auto            it = _pending.find(requestID);
            if (it == _pending.end())
            {
                return; // 已超时/断线清理，丢弃
            }
            call = std::move(it->second);
            _pending.erase(it);
        }

        if (call.deserializeAndCallback)
        {
            EnqueueCallback(
                [cb = std::move(call.deserializeAndCallback), body = std::vector<uint8>(body, body + len)]() {
                    cb(body.data(), body.size());
                });
        }
    }

    void RPCClient::OnConnectionLost()
    {
        std::vector<std::function<void()>> errorCbs;
        {
            std::lock_guard lock(_pendingMtx);
            for (auto &[id, call] : _pending)
            {
                if (call.onError)
                {
                    errorCbs.push_back(std::move(call.onError));
                }
            }
            _pending.clear();
        }
        for (auto &cb : errorCbs)
        {
            EnqueueCallback(std::move(cb));
        }
    }

    void RPCClient::ProcessTimeouts()
    {
        auto                               now = std::chrono::steady_clock::now();
        std::vector<std::function<void()>> timedOut;

        {
            std::lock_guard lock(_pendingMtx);
            for (auto it = _pending.begin(); it != _pending.end();)
            {
                if (now > it->second.deadline)
                {
                    if (it->second.onError)
                    {
                        timedOut.push_back(std::move(it->second.onError));
                    }
                    it = _pending.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (auto &cb : timedOut)
        {
            EnqueueCallback(std::move(cb));
        }
    }

    void RPCClient::StartTimeoutChecker(asio::io_context &ioCtx)
    {
        if (_timerStarted)
        {
            return;
        }
        _timerStarted = true;

        _timeoutTimer = std::make_unique<asio::steady_timer>(ioCtx);

        ScheduleTimeoutCheck();
    }

    void RPCClient::ScheduleTimeoutCheck()
    {
        if (!_timeoutTimer)
        {
            return;
        }
        _timeoutTimer->expires_after(std::chrono::milliseconds(100));
        _timeoutTimer->async_wait([this](const asio::error_code &ec) {
            if (ec)
            {
                return; // io_context 停止或定时器取消
            }
            ProcessTimeouts();
            ScheduleTimeoutCheck(); // 递归重设
        });
    }

    void RPCClient::EnqueueCallback(std::function<void()> fn)
    {
        if (_useLogicThread)
        {
            // 投递到 MPSCQueue，等 LogicThread::ProcessRPCResponses() 消费
            _completedQueue.Enqueue(Completed {std::move(fn)});
        }
        else
        {
            // IO 线程直跑（Center/Social 侧）
            fn();
        }
    }

} // namespace MMO
