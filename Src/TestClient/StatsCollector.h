/**
 * @file StatsCollector.h
 * @brief TestClient 统计收集器——线程安全的延迟/吞吐/错误率
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "Common/Core/Types.h"

namespace MMO::TestClient
{

    /**
     * @brief 单个操作的延迟记录
     */
    struct LatencyRecord
    {
        std::string               operation; // "LoginAuth", "EnterWorld", "MoveReq"
        std::chrono::microseconds latency;
        bool                      success   = true;
        uint32                    errorCode = 0;
    };

    /**
     * @brief 线程安全的统计收集器
     *
     * 所有 VirtualClient 共享同一个 StatsCollector 实例。
     * 原子计数器保证无锁写入，Snapshot() 用于周期性输出报告。
     */
    class StatsCollector
    {
    public:
        StatsCollector() : _startTime(std::chrono::steady_clock::now())
        {
        }

        // ── 计数器 ──

        void RecordLoginAttempt(bool success, uint32 errorCode = 0)
        {
            if (success)
            {
                _loginSuccess.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                _loginFail.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void RecordEnterWorldAttempt(bool success, uint32 errorCode = 0)
        {
            if (success)
            {
                _enterWorldSuccess.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                _enterWorldFail.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void RecordHeartbeatSent()
        {
            _heartbeatSent.fetch_add(1, std::memory_order_relaxed);
        }

        void RecordHeartbeatRcvd()
        {
            _heartbeatRcvd.fetch_add(1, std::memory_order_relaxed);
        }

        void RecordMoveSent()
        {
            _moveSent.fetch_add(1, std::memory_order_relaxed);
        }

        void RecordMoveRcvd()
        {
            _moveRcvd.fetch_add(1, std::memory_order_relaxed);
        }

        void RecordDisconnect()
        {
            _disconnects.fetch_add(1, std::memory_order_relaxed);
        }

        // ── 延迟记录 ──

        void
        RecordLatency(const std::string &operation, std::chrono::microseconds latency, bool success = true)
        {
            std::lock_guard lock(_latencyMutex);
            _latencyRecords.push_back({operation, latency, success});
        }

        // ── 快照查询 ──

        struct Snapshot
        {
            uint32 loginSuccess      = 0;
            uint32 loginFail         = 0;
            uint32 enterWorldSuccess = 0;
            uint32 enterWorldFail    = 0;
            uint32 heartbeatSent     = 0;
            uint32 heartbeatRcvd     = 0;
            uint32 moveSent          = 0;
            uint32 moveRcvd          = 0;
            uint32 disconnects       = 0;
            uint32 activeClients     = 0;
            double elapsedSec        = 0;
        };

        Snapshot GetSnapshot() const
        {
            Snapshot s;
            s.loginSuccess      = _loginSuccess.load(std::memory_order_relaxed);
            s.loginFail         = _loginFail.load(std::memory_order_relaxed);
            s.enterWorldSuccess = _enterWorldSuccess.load(std::memory_order_relaxed);
            s.enterWorldFail    = _enterWorldFail.load(std::memory_order_relaxed);
            s.heartbeatSent     = _heartbeatSent.load(std::memory_order_relaxed);
            s.heartbeatRcvd     = _heartbeatRcvd.load(std::memory_order_relaxed);
            s.moveSent          = _moveSent.load(std::memory_order_relaxed);
            s.moveRcvd          = _moveRcvd.load(std::memory_order_relaxed);
            s.disconnects       = _disconnects.load(std::memory_order_relaxed);
            s.activeClients     = _activeClients.load(std::memory_order_relaxed);

            auto now     = std::chrono::steady_clock::now();
            s.elapsedSec = std::chrono::duration<double>(now - _startTime).count();
            return s;
        }

        void SetActiveClients(uint32 count)
        {
            _activeClients.store(count, std::memory_order_relaxed);
        }

        std::vector<LatencyRecord> DrainLatencyRecords()
        {
            std::lock_guard lock(_latencyMutex);
            auto            records = std::move(_latencyRecords);
            _latencyRecords.clear();
            return records;
        }

    private:
        std::chrono::steady_clock::time_point _startTime;

        std::atomic<uint32> _loginSuccess {0};
        std::atomic<uint32> _loginFail {0};
        std::atomic<uint32> _enterWorldSuccess {0};
        std::atomic<uint32> _enterWorldFail {0};
        std::atomic<uint32> _heartbeatSent {0};
        std::atomic<uint32> _heartbeatRcvd {0};
        std::atomic<uint32> _moveSent {0};
        std::atomic<uint32> _moveRcvd {0};
        std::atomic<uint32> _disconnects {0};
        std::atomic<uint32> _activeClients {0};

        std::mutex                 _latencyMutex;
        std::vector<LatencyRecord> _latencyRecords;
    };

} // namespace MMO::TestClient
