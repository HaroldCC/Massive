/**
 * @file ClientManager.cpp
 * @brief ClientManager 实现
 */
#include <asio/post.hpp>

#include "TestClient/ClientManager.h"
#include "TestClient/VirtualClient.h"
#include "TestClient/Scenarios/LoginFlowScenario.h"
#include "Common/Log/Log.h"
#include "Common/Network/IOContextPool.h"

namespace MMO::TestClient
{

    ClientManager::ClientManager(const TestClientConfig &cfg,
                                IOContextPool           &pool,
                                StatsCollector          &stats)
        : _cfg(cfg)
        , _pool(pool)
        , _stats(stats)
    {
    }

    void ClientManager::Start()
    {
        _running    = true;
        _startTime      = std::chrono::steady_clock::now();
        _lastReportTime = _startTime;
        _lastSpawnTime  = _startTime;

        Log::Info("ClientManager: starting {} clients ({} spawn/s)",
                 _cfg.clientCount, _cfg.spawnRatePerSec);

        SpawnBatch();
    }

    void ClientManager::Stop()
    {
        _running = false;
        for (auto &client : _clients)
        {
            if (client)
            {
                client->Disconnect();
            }
        }
        _clients.clear();
    }

    void ClientManager::SpawnBatch()
    {
        if (!_running)
        {
            return;
        }

        uint32 toSpawn = std::min(
            _cfg.clientCount - _spawnedCount,
            static_cast<uint32>(_cfg.spawnRatePerSec > 0 ? _cfg.spawnRatePerSec : 1));

        for (uint32 i = 0; i < toSpawn; ++i)
        {
            uint32 id   = _spawnedCount;
            std::string username = _cfg.usernamePrefix + std::to_string(id);

            // 创建 Scenario
            auto scenario = std::make_unique<LoginFlowScenario>(
                _cfg.heartbeatIntervalSec,
                _cfg.moveIntervalMs,
                static_cast<float>(_cfg.moveSpeed),
                _cfg.moveRadius,
                _cfg.durationSec);

            auto client = std::make_shared<VirtualClient>(
                id,
                username,
                _cfg.password,
                _cfg.loginHost,
                _cfg.loginPort,
                _stats,
                _pool,
                std::move(scenario));

            _clients.push_back(client);

            // 用 asio::post 在 IO 线程中启动
            auto &ctx = _pool.GetNextContext();
            asio::post(ctx, [client]() {
                client->Start();
            });

            _spawnedCount++;
            _stats.SetActiveClients(_spawnedCount);
        }

        Log::Info("ClientManager: spawned {} / {} clients", _spawnedCount, _cfg.clientCount);
    }

    void ClientManager::Tick()
    {
        if (!_running)
        {
            return;
        }

        auto now = std::chrono::steady_clock::now();

        // 按速率生成
        auto sinceSpawn = std::chrono::duration<double>(now - _lastSpawnTime).count();
        if (_spawnedCount < _cfg.clientCount && sinceSpawn >= 1.0)
        {
            _lastSpawnTime = now;
            SpawnBatch();
        }

        // 定期报告
        auto sinceReport = std::chrono::duration<double>(now - _lastReportTime).count();
        if (sinceReport >= 5.0)
        {
            _lastReportTime = now;
            PrintReport();
        }
    }

    void ClientManager::PrintReport()
    {
        auto snap = _stats.GetSnapshot();

        Log::Info("═══ TestClient Report (t={:.1f}s) ═══", snap.elapsedSec);
        Log::Info("  Active:     {} / {}",
                 snap.activeClients, _cfg.clientCount);
        Log::Info("  Login:      {} OK / {} FAIL",
                 snap.loginSuccess, snap.loginFail);
        Log::Info("  EnterWorld: {} OK / {} FAIL",
                 snap.enterWorldSuccess, snap.enterWorldFail);
        Log::Info("  Heartbeat:  {} sent / {} rcvd",
                 snap.heartbeatSent, snap.heartbeatRcvd);
        Log::Info("  Move:       {} sent / {} rcvd",
                 snap.moveSent, snap.moveRcvd);
        Log::Info("  Disconnects: {}", snap.disconnects);

        // 延迟统计
        auto records = _stats.DrainLatencyRecords();
        if (!records.empty())
        {
            // 简单均值
            std::unordered_map<std::string, std::pair<uint64, uint32>> latencyMap;
            for (auto &r : records)
            {
                auto &[total, count] = latencyMap[r.operation];
                total += r.latency.count();
                count++;
            }
            for (auto &[op, tc] : latencyMap)
            {
                Log::Info("  Latency {}: avg={:.2f}ms ({} samples)",
                         op, tc.first / 1000.0 / tc.second, tc.second);
            }
        }
    }

} // namespace MMO::TestClient
