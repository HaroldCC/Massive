/**
 * @file main.cpp
 * @brief WorldServer 进程入口
 *
 * 启动流程：
 *   1. Load 配置（toml + LSS 密钥文件）
 *   2. Log::Init + InstallStackTrace + SetCrashDumpDirectory
 *   3. InstallGracefulShutdown（SIGINT/SIGTERM → WorldServer::Stop）
 *   4. DBWorkerPool::Init
 *   5. WorldServer::Init（CenterClient + GateAcceptor + LogicThread）
 *   6. WorldServer::Run（ioPool.Start + 等待 LogicThread）
 *   7. server.Run() 返回后 ShutdownLog 保日志落盘
 *
 * 用法：
 *   WorldServer.exe
 *   WorldServer.exe --config-path "F:/Dev/Massive/Config/world.toml"
 *   WorldServer.exe --config-path "..." --key-path "..."
 */
#include "World/WorldConfig.h"
#include "World/WorldServer.h"

#include "Common/Core/Args.h"
#include "Common/Log/GracefulShutdown.h"
#include "Common/Core/Stacktrace.h"
#include "Common/DB/DBWorkerPool.h"
#include "Common/Log/Log.h"

int main(int argc, char **argv)
{
    MMO::Args args(argc, argv);

    auto configPath = args.Get("--config-path", "Config/world.toml");
    auto keyPath    = args.Get("--key-path", "Config/login.key");

    auto cfg = MMO::WorldConfig::Load(configPath, keyPath);
    if (!cfg)
    {
        return 1;
    }

    MMO::Log::Init("world", cfg->log);
    MMO::InstallStackTrace(argv[0]);
    MMO::SetCrashDumpDirectory(cfg->log.logDir.c_str());

    MMO::WorldServer server;
    MMO::InstallGracefulShutdown(
        [&server] {
            server.Stop();
        },
        "WorldServer");

    MMO::DB::DBWorkerPool::Init(cfg->database.workerCount, cfg->database.connString);

    if (!server.Init(*cfg))
    {
        MMO::Log::Error("WorldServer init failed");
        return 1;
    }

    server.Run();

    // Run 返回后确保日志落盘（异步队列排空）
    MMO::ShutdownLog();
    return 0;
}
