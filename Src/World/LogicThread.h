/**
 * @file LogicThread.h
 * @brief WorldServer 单线程逻辑循环
 *
 * 20ms 固定 Tick：
 *   ProcessMessages → TimingWheel.Tick → ProcessDB → ProcessRPC
 *   → Tick(elapsed) → FlushOutgoing
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "Common/Timer/TimingWheel.h"

#include "World/WorldSession.h"

namespace MMO
{

    /**
     * @brief 单线程逻辑循环
     *
     * 设计文档 §4.2 — 20ms Tick + 防死亡螺旋 Clamp。
     */
    class LogicThread
    {
    public:
        using TickCallback = std::function<void(float dtSeconds)>;
        using DispatchCallback =
            std::function<void(uint32 sessionID, WorldSession &ws, const LogicMessage &msg)>;

        LogicThread();

        /**
         * @brief 启动逻辑线程
         * @param sessions          WorldServer::_sessions 引用
         * @param sessionsMtx       WorldServer::_sessionsMtx 引用
         * @param onTick            游戏逻辑回调（DasLang 脚本入口）
         * @param onMessage         消息分发回调（按 msgID 查表 → handler）
         * @param preProcess        可选：ProcessMessages 前回调（RPCClient::ProcessTimeouts 等）
         * @param postFlush         可选：Flush 后回调（CenterClient::SendHeartbeat 等）
         */
        void Start(std::unordered_map<uint32, WorldSession> *sessions,
                   std::shared_mutex                        *sessionsMtx,
                   TickCallback                              onTick,
                   DispatchCallback                          onMessage,
                   std::function<void()>                     preProcess = nullptr,
                   std::function<void()>                     postFlush  = nullptr);

        void Stop();

        bool IsRunning() const
        {
            return _running.load(std::memory_order_acquire);
        }

        // 供外部调用的统计
        size_t LastTickProcessed() const
        {
            return _lastProcessed.load(std::memory_order_relaxed);
        }

        // 供 WorldServer 注册定时器（LogicThread 独占，非线程安全）
        TimingWheel &GetTimingWheel()
        {
            return _timingWheel;
        }

    private:
        void RunLoop(std::unordered_map<uint32, WorldSession> *sessions,
                     std::shared_mutex                        *sessionsMtx,
                     TickCallback                              onTick,
                     DispatchCallback                          onMessage,
                     std::function<void()>                     preProcess,
                     std::function<void()>                     postFlush);

        void ProcessMessages(std::unordered_map<uint32, WorldSession> *sessions,
                             std::shared_mutex                        *sessionsMtx,
                             DispatchCallback                          onMessage,
                             size_t                                    limit);

        TimingWheel       _timingWheel;
        std::thread       _thread;
        std::atomic<bool> _running {false};
        std::atomic<bool> _stopped {false};

        std::atomic<size_t> _lastProcessed {0};

        // 动态入口门控——根据 Tick 负载自动调整每 Tick 处理的消息数
        uint32 _currentMsgLimit = kMaxMessagesPerTick;

        float _accumulator = 0.0f; // 追帧欠账（秒）

        static constexpr size_t kMaxMessagesPerTick = 1000;
        static constexpr auto   kMaxElapsed         = std::chrono::milliseconds(50);

        // 固定模拟步长（秒）20ms
        static constexpr float kFixedDeltaTime = 0.02f; // 20ms

        // 单帧追帧上限——防死亡螺旋
        static constexpr uint32 kMaxCatchupSteps = 3;
    };

} // namespace MMO
