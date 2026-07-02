/**
 * @file ClientManager.h
 * @brief 虚拟客户端生命周期管理
 *
 * 负责：
 *   1. 批量创建 VirtualClient
 *   2. 按速率逐步启动（spawnRatePerSec）
 *   3. 定期输出统计报告
 *   4. 超时或全部完成后停止
 */
#pragma once

#include <memory>
#include <vector>

#include "Common/Core/Types.h"

#include "TestClient/TestClientConfig.h"
#include "TestClient/StatsCollector.h"

namespace MMO
{
    class IOContextPool;
} // namespace MMO

namespace MMO::TestClient
{

    class VirtualClient;

    class ClientManager
    {
    public:
        ClientManager(const TestClientConfig &cfg, IOContextPool &pool, StatsCollector &stats);

        ClientManager(const ClientManager &)            = delete;
        ClientManager &operator=(const ClientManager &) = delete;

        /// 启动所有客户端（按 spawnRatePerSec 分批）
        void Start();

        /// 停止所有客户端
        void Stop();

        /// 是否正在运行
        bool IsRunning() const
        {
            return _running;
        }

        /// 主线程 Tick（调用 StatsCollector 输出报告等）
        void Tick();

    private:
        void SpawnBatch();
        void PrintReport();

        const TestClientConfig &_cfg;
        IOContextPool          &_pool;
        StatsCollector         &_stats;

        std::vector<std::shared_ptr<VirtualClient>> _clients;
        uint32                                      _spawnedCount = 0;
        bool                                        _running      = false;

        std::chrono::steady_clock::time_point _startTime;
        std::chrono::steady_clock::time_point _lastReportTime;
        std::chrono::steady_clock::time_point _lastSpawnTime;
    };

} // namespace MMO::TestClient
