/**
 * @file LogicThread.cpp
 * @brief LogicThread 实现—20ms 主循环 + Per-Session inbox DrainAll
 */

#include "World/LogicThread.h"
#include "Common/DB/DBWorkerPool.h"
#include "Common/Log/Log.h"

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace MMO
{

    LogicThread::LogicThread() = default;

    void LogicThread::Start(std::unordered_map<uint32, WorldSession> *sessions,
                            std::shared_mutex                        *sessionsMtx,
                            TickCallback                              onTick,
                            DispatchCallback                          onMessage,
                            std::function<void()>                     preProcess,
                            std::function<void()>                     postFlush)
    {
        _stopped.store(false, std::memory_order_release);

        _thread = std::thread(&LogicThread::RunLoop,
                              this,
                              sessions,
                              sessionsMtx,
                              std::move(onTick),
                              std::move(onMessage),
                              std::move(preProcess),
                              std::move(postFlush));
    }

    void LogicThread::Stop()
    {
        _stopped.store(true, std::memory_order_release);
        if (_thread.joinable())
        {
            _thread.join();
        }
        _running.store(false, std::memory_order_release);
    }

    void LogicThread::RunLoop(std::unordered_map<uint32, WorldSession> *sessions,
                              std::shared_mutex                        *sessionsMtx,
                              TickCallback                              onTick,
                              DispatchCallback                          onMessage,
                              std::function<void()>                     preProcess,
                              std::function<void()>                     postFlush)
    {
        _running.store(true, std::memory_order_release);

        auto lastTime = std::chrono::steady_clock::now();

        Log::Info("LogicThread: started (tickInterval={}ms, maxElapsed={}ms)",
                  kTickInterval.count(),
                  kMaxElapsed.count());

        while (!_stopped.load(std::memory_order_acquire))
        {
            auto tickStart = std::chrono::steady_clock::now();

            // ── Phase 0: 动态计算本 Tick 入口限制 ──
            // 利用上一 Tick 的耗时反馈调整：超载时收缩入口，空闲时恢复
            // 这样不需要 skip Tick，不改间隔，自然挡住积压
            size_t msgLimit = _currentMsgLimit;

            // ── Phase 1: 预处理（RPC 超时等） ──
            if (preProcess)
            {
                preProcess();
            }

            ProcessMessages(sessions, sessionsMtx, onMessage, msgLimit);

            // ── Phase 2: 定时器 ──
            _timingWheel.Tick();

            // ── Phase 3: DB 回调 ──
            DB::DBWorkerPool::Instance().ProcessCallbacks();

            // ── Phase 4: 游戏逻辑（传剩余预算给业务层） ──
            auto now     = std::chrono::steady_clock::now();
            auto elapsed = now - lastTime;
            if (elapsed > kMaxElapsed)
            {
                elapsed = kMaxElapsed;
            }
            lastTime = now;

            // 计算剩余预算：离 20ms 还有多少时间，给业务层自己决定做多少事
            auto usedInTick = std::chrono::steady_clock::now() - tickStart;
            auto budget     = kTickInterval - usedInTick;
            if (budget < std::chrono::milliseconds(5))
            {
                budget = std::chrono::milliseconds(5); // 最少给 5ms
            }

            onTick(std::chrono::duration_cast<std::chrono::milliseconds>(budget));

            // ── Phase 6: 出站刷新 ──
            if (postFlush)
            {
                postFlush();
            }

            // ── Phase 7: 负载反馈调整下一 Tick 入口 ──
            auto tickCost = std::chrono::steady_clock::now() - tickStart;
            if (tickCost > kTickInterval * 0.8)
            {
                // 超载了 → 收缩入口
                _currentMsgLimit = std::max(100u, static_cast<uint32>(_currentMsgLimit * 0.8));
                Log::Debug("LogicThread: overloaded ({}ms), shrinking limit to {}",
                           std::chrono::duration_cast<std::chrono::milliseconds>(tickCost).count(),
                           _currentMsgLimit);
            }
            else if (tickCost < kTickInterval * 0.3 && _currentMsgLimit < kMaxMessagesPerTick)
            {
                // 空闲了 → 逐步恢复入口
                _currentMsgLimit = std::min(static_cast<uint32>(kMaxMessagesPerTick),
                                            static_cast<uint32>(_currentMsgLimit * 1.1));
            }

            // ── sleep 到下一个 Tick ──
            auto tickEnd = std::chrono::steady_clock::now();
            auto workDur = tickEnd - tickStart;
            if (workDur < kTickInterval)
            {
                std::this_thread::sleep_for(kTickInterval - workDur);
            }
            else
            {
                Log::Warn(
                    "LogicThread: tick overran by {}ms",
                    std::chrono::duration_cast<std::chrono::milliseconds>(workDur - kTickInterval).count());
            }
        }

        _running.store(false, std::memory_order_release);
        Log::Info("LogicThread: stopped");
    }

    void LogicThread::ProcessMessages(std::unordered_map<uint32, WorldSession> *sessions,
                                      std::shared_mutex                        *sessionsMtx,
                                      DispatchCallback                          onMessage,
                                      size_t                                    limit)
    {
        if (!sessions || !sessionsMtx)
        {
            return;
        }

        size_t processed = 0;
        size_t totalMsgs = 0;

        // 遍历所有活跃 Session，Drain 其 Per-Session inbox
        // LogicThread 读遍历，IO 线程通过 shared_lock 并发读，需要加共享锁避免 data race
        std::shared_lock lock(*sessionsMtx);
        for (auto &[sessionID, ws] : *sessions)
        {
            if (ws.disconnected)
            {
                continue;
            }

            std::vector<LogicMessage> batch;
            ws.inbox.DrainAll(batch);
            totalMsgs += batch.size();

            for (auto &msg : batch)
            {
                if (processed >= limit)
                {
                    Log::Warn("LogicThread: message limit reached ({}), remaining={}",
                              limit,
                              totalMsgs - processed);
                    _lastProcessed.store(processed, std::memory_order_relaxed);
                    return;
                }

                ws.lastRecvTime = msg.recvTime;
                onMessage(sessionID, ws, msg);
                processed++;
            }
        }

        _lastProcessed.store(processed, std::memory_order_relaxed);

        if (totalMsgs > kMaxMessagesPerTick * 2)
        {
            Log::Warn("LogicThread: total queued messages={}", totalMsgs);
        }
    }

} // namespace MMO
