/**
 * @file main.cpp
 * @brief GateServer 进程入口
 *
 * 启动流程：
 *   1. Load 配置（gate.toml）
 *   2. Log::Init + InstallStackTrace + SetCrashDumpDirectory
 *   3. InstallGracefulShutdown（SIGINT/SIGTERM → GateServer::Stop）
 *   4. GateServer::Init（IOContextPool + TCPAcceptor + 连 World）
 *   5. GateServer::Run（ioPool.Start + 定时超时检查）
 *   6. server.Run() 返回后 ShutdownLog 保日志落盘
 *
 * 用法：
 *   GateServer.exe
 *   GateServer.exe --config-path "F:/Dev/Massive/Config/gate.toml"
 */
#include "Gate/GateConfig.h"
#include "Gate/GateServer.h"

#include "Common/Core/Args.h"
#include "Common/Log/GracefulShutdown.h"
#include "Common/Core/Stacktrace.h"
#include "Common/Log/Log.h"

int main(int argc, char **argv)
{
    MMO::Args args(argc, argv);

    auto configPath = args.Get("--config-path", "Config/gate.toml");

    auto cfg = MMO::GateConfig::Load(configPath);
    if (!cfg)
    {
        return 1;
    }

    MMO::Log::Init("gate", cfg->log);
    MMO::InstallStackTrace(argv[0]);
    MMO::SetCrashDumpDirectory(cfg->log.logDir.c_str());

    MMO::GateServer server;
    MMO::InstallGracefulShutdown(
        [&server] {
            server.Stop();
        },
        "GateServer");

    if (!server.Init(*cfg))
    {
        MMO::Log::Error("GateServer init failed");
        return 1;
    }

    server.Run();

    // Run 返回后确保日志落盘（异步队列排空）
    MMO::ShutdownLog();
    return 0;
}
