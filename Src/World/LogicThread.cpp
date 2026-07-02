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

        auto           lastTime     = std::chrono::steady_clock::now();
        constexpr auto tickInterval = 20ms;
        constexpr auto maxElapsed   = 50ms;

        Log::Info("LogicThread: started (tickInterval={}ms, clamp={}ms)",
                  tickInterval.count(),
                  maxElapsed.count());

        while (!_stopped.load(std::memory_order_acquire))
        {
            // ── Phase 1: 预处理（RPC 超时等） ──
            if (preProcess)
            {
                preProcess();
            }

            ProcessMessages(sessions, onMessage);

            // ── Phase 2: 定时器 ──
            _timingWheel.Tick();

            // ── Phase 3: DB 回调 ──
            DB::DBWorkerPool::Instance().ProcessCallbacks();

            // ── Phase 4: 游戏逻辑 ──
            auto now     = std::chrono::steady_clock::now();
            auto elapsed = now - lastTime;
            if (elapsed > maxElapsed)
            {
                elapsed = maxElapsed;
            }
            lastTime = now;

            onTick(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed));

            // ── Phase 6: 出站刷新 ──
            if (postFlush)
            {
                postFlush();
            }

            // ── sleep 到下一个 Tick ──
            auto tickEnd = std::chrono::steady_clock::now();
            auto workDur = tickEnd - now;
            if (workDur < tickInterval)
            {
                std::this_thread::sleep_for(tickInterval - workDur);
            }
            else
            {
                Log::Warn(
                    "LogicThread: tick overran by {}ms",
                    std::chrono::duration_cast<std::chrono::milliseconds>(workDur - tickInterval).count());
            }
        }

        _running.store(false, std::memory_order_release);
        Log::Info("LogicThread: stopped");
    }

    void LogicThread::ProcessMessages(std::unordered_map<uint32, WorldSession> *sessions,
                                      DispatchCallback                          onMessage)
    {
        if (!sessions)
        {
            return;
        }

        size_t processed = 0;
        size_t totalMsgs = 0;

        // 遍历所有活跃 Session，Drain 其 Per-Session inbox
        // LogicThread 是唯一消费者，sessions 不会被并发修改
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
                if (processed >= kMaxMessagesPerTick)
                {
                    Log::Warn("LogicThread: kMaxMessagesPerTick reached ({}), remaining={}",
                              kMaxMessagesPerTick,
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
