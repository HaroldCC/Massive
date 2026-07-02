/**
 * @file main.cpp
 * @brief TestClient 入口——多客户端压测工具
 *
 * 用法：
 *   TestClient.exe --config Config/testclient.toml
 *   TestClient.exe --help
 */
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "TestClient/ClientManager.h"
#include "TestClient/TestClientConfig.h"
#include "TestClient/StatsCollector.h"

#include "Common/Log/Log.h"
#include "Common/Network/IOContextPool.h"

using namespace std::chrono_literals;
using namespace MMO;
using namespace MMO::TestClient;

namespace
{

    void PrintUsage()
    {
        std::cout << R"(TestClient — MMO Client Simulator

Usage:
  TestClient.exe --config CONFIG_PATH
  TestClient.exe --config CONFIG_PATH --count N --duration SEC

Options:
  --config PATH    TOML 配置文件路径（必需）
  --count N        覆盖配置文件中的 client_count
  --duration SEC   覆盖配置文件中的 duration_sec
  --help           显示帮助

Config file format (testclient.toml):
  [server]
  host = "127.0.0.1"
  port = 9000

  [clients]
  count = 1
  spawn_rate_per_sec = 1.0
  duration_sec = 60

  [behavior]
  heartbeat_interval_sec = 5
  move_interval_ms = 200
  move_speed = 5.0
  move_radius = 50.0

  [account]
  username_prefix = "testuser"
  password = "test123"

  [debug]
  verbose = false
)";
    }

} // namespace

int main(int argc, char *argv[])
{
    std::string configPath;

    // 解析 CLI 参数
    uint32 cliCount    = 0;
    uint32 cliDuration = 0;

    for (int32 i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--help") == 0)
        {
            PrintUsage();
            return 0;
        }
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc)
        {
            configPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc)
        {
            cliCount = static_cast<uint32>(std::stoul(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
        {
            cliDuration = static_cast<uint32>(std::stoul(argv[++i]));
        }
    }

    if (configPath.empty())
    {
        std::cerr << "Error: --config is required.\n";
        PrintUsage();
        return 1;
    }

    // 加载配置
    auto cfgOpt = TestClientConfig::Load(configPath);
    if (!cfgOpt)
    {
        std::cerr << "Error: failed to load config from " << configPath << "\n";
        return 1;
    }
    auto cfg = *cfgOpt;

    // CLI 覆盖
    if (cliCount > 0)
    {
        cfg.clientCount = cliCount;
    }
    if (cliDuration > 0)
    {
        cfg.durationSec = cliDuration;
    }

    // 初始化日志
    Log::Config logCfg;
    logCfg.level  = cfg.verbose ? ELogLevel::Debug : ELogLevel::Info;
    logCfg.logDir = "logs";
    Log::Init("testclient", logCfg.level, logCfg.logDir);

    Log::Info("TestClient: {} clients, {}s duration, {} spawn/s",
              static_cast<int32>(cfg.clientCount),
              static_cast<int32>(cfg.durationSec),
              static_cast<int32>(cfg.spawnRatePerSec));

    // IO 线程池（模拟客户端的 N）
    int32         ioThreads = std::max(static_cast<int32>(2), static_cast<int32>(cfg.clientCount / 500 + 1));
    IOContextPool ioPool(static_cast<size_t>(ioThreads));
    ioPool.Start();

    // 统计
    StatsCollector stats;

    // 管理器
    ClientManager manager(cfg, ioPool, stats);
    manager.Start();

    // 主循环
    auto startTime = std::chrono::steady_clock::now();

    while (manager.IsRunning())
    {
        manager.Tick();
        std::this_thread::sleep_for(500ms);

        // 检查超时
        if (cfg.durationSec > 0)
        {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startTime)
                    .count();
            if (static_cast<uint32>(elapsed) >= cfg.durationSec)
            {
                Log::Info("TestClient: duration reached, stopping...");
                break;
            }
        }
    }

    manager.Stop();
    ioPool.Stop();

    // 最终报告
    auto snap = stats.GetSnapshot();
    Log::Info("═══ Final Report ═══");
    Log::Info("  Login:      {} success / {} failed", snap.loginSuccess, snap.loginFail);
    Log::Info("  EnterWorld: {} success / {} failed", snap.enterWorldSuccess, snap.enterWorldFail);
    Log::Info("  Heartbeat:  {} sent / {} rcvd", snap.heartbeatSent, snap.heartbeatRcvd);
    Log::Info("  Move:       {} sent / {} rcvd", snap.moveSent, snap.moveRcvd);
    Log::Info("  Disconnects: {}", snap.disconnects);

    return 0;
}
