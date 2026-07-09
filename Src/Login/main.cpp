/**
 * @file main.cpp
 * @brief LoginServer 进程入口
 *
 * 启动流程：
 *   1. Load 配置（toml + LSS 密钥文件）
 *   2. Log::Init + InstallStackTrace + SetCrashDumpDirectory
 *   3. InstallGracefulShutdown（SIGINT/SIGTERM → LoginServer::Stop）
 *   4. DBWorkerPool::Init
 *   5. LoginServer::Init（IOContextPool + TCPAcceptor + MessageDispatcher）
 *   6. LoginServer::Run（ioPool.Start + 主线程轮询 DB 回调）
 *   7. server.Run() 返回后 ShutdownLog 保日志落盘
 *
 * 用法：
 *   LoginServer.exe
 *   LoginServer.exe --config-path "F:/Dev/Massive/Config/login.toml"
 *   LoginServer.exe --config-path "..." --key-path "..."
 */
#include "Login/LoginConfig.h"
#include "Login/LoginServer.h"

#include "Common/Core/Args.h"
#include "Common/Log/GracefulShutdown.h"
#include "Common/Core/Stacktrace.h"
#include "Common/DB/DBWorkerPool.h"
#include "Common/Log/Log.h"

int main(int argc, char **argv)
{
    MMO::Args args(argc, argv);

    auto configPath = args.Get("--config-path", "Config/login.toml");
    auto keyPath    = args.Get("--key-path", "Config/login.key");

    auto cfg = MMO::LoginConfig::Load(configPath, keyPath);
    if (!cfg)
    {
        return 1;
    }

    MMO::Log::Init("login", cfg->log);
    MMO::InstallStackTrace(argv[0]);
    MMO::SetCrashDumpDirectory(cfg->log.logDir.c_str());
    MMO::DB::DBWorkerPool::Init(cfg->database.workerCount, cfg->database.connString);

    MMO::LoginServer server;
    MMO::InstallGracefulShutdown(
        [&server] {
            server.Stop();
        },
        "LoginServer");

    if (!server.Init(*cfg))
    {
        MMO::Log::Error("LoginServer init failed");
        return 1;
    }

    server.Run();

    // Run 返回后确保日志落盘（异步队列排空）
    MMO::ShutdownLog();
    return 0;
}
