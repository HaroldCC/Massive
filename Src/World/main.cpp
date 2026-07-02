/**
 * @file main.cpp
 * @brief WorldServer 进程入口
 *
 * 启动流程：
 *   1. Load 配置（toml + LSS 密钥文件）
 *   2. Log::Init + InstallStackTrace
 *   3. DBWorkerPool::Init
 *   4. WorldServer::Init（CenterClient + GateAcceptor + LogicThread）
 *   5. WorldServer::Run（ioPool.Start + 等待 LogicThread）
 *
 * 用法：
 *   WorldServer.exe
 *   WorldServer.exe --config-path "F:/Dev/Massive/Config/world.toml"
 *   WorldServer.exe --config-path "..." --key-path "..."
 */
#include "World/WorldConfig.h"
#include "World/WorldServer.h"

#include "Common/Core/Args.h"
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
    MMO::DB::DBWorkerPool::Init(cfg->database.workerCount, cfg->database.connString);

    MMO::WorldServer server;
    if (!server.Init(*cfg))
    {
        MMO::Log::Error("WorldServer init failed");
        return 1;
    }

    server.Run();
    return 0;
}
