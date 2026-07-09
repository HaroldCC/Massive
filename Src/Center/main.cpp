/**
 * @file main.cpp
 * @brief CenterServer 进程入口
 *
 * 启动流程：
 *   1. Load 配置（toml + LSS 密钥文件）
 *   2. Log::Init + InstallStackTrace + SetCrashDumpDirectory
 *   3. InstallGracefulShutdown（SIGINT/SIGTERM → CenterServer::Stop）
 *   4. CenterServer::Init（IOContextPool + TCPAcceptor + MessageDispatcher）
 *   5. CenterServer::Run（ioPool.Start + 主线程阻塞等待）
 *   6. server.Run() 返回后 ShutdownLog 保日志落盘
 *
 * 用法：
 *   CenterServer.exe
 *   CenterServer.exe --config-path "F:/Dev/Massive/Config/center.toml"
 */
#include "Center/CenterConfig.h"
#include "Center/CenterServer.h"
#include "Common/Core/Args.h"
#include "Common/Log/GracefulShutdown.h"
#include "Common/Core/Stacktrace.h"
#include "Common/Log/Log.h"

int main(int argc, char **argv)
{
    MMO::Args args(argc, argv);

    auto configPath = args.Get("--config-path", "Config/center.toml");

    auto cfg = MMO::CenterConfig::Load(configPath);
    if (!cfg)
    {
        return 1;
    }

    MMO::Log::Init("center", cfg->log);
    MMO::InstallStackTrace(argv[0]);
    MMO::SetCrashDumpDirectory(cfg->log.logDir.c_str());

    MMO::CenterServer server;
    MMO::InstallGracefulShutdown(
        [&server] {
            server.Stop();
        },
        "CenterServer");

    if (!server.Init(*cfg))
    {
        MMO::Log::Error("CenterServer init failed");
        return 1;
    }

    server.Run();

    // Run 返回后确保日志落盘（异步队列排空）
    MMO::ShutdownLog();
    return 0;
}
