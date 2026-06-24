/**
 * @file main.cpp
 * @brief LoginServer 进程入口
 *
 * 启动流程：
 *   1. Load 配置（toml + LSS 密钥文件）
 *   2. Log::Init / DBWorkerPool::Init
 *   3. LoginServer::Init（IOContextPool + TCPAcceptor + MessageDispatcher）
 *   4. LoginServer::Run（ioPool.Start + 主线程轮询 DB 回调）
 */
#include "Login/LoginConfig.h"
#include "Login/LoginServer.h"

#include "Common/DB/DBWorkerPool.h"
#include "Common/Log/Log.h"

int main()
{
    auto cfg = MMO::LoginConfig::Load("Config/login.toml");
    if (!cfg)
    {
        return 1;
    }

    MMO::Log::Init("login", cfg->log);
    MMO::DB::DBWorkerPool::Init(cfg->database.workerCount, cfg->database.connString);

    MMO::LoginServer server;
    if (!server.Init(*cfg))
    {
        MMO::Log::Error("LoginServer init failed");
        return 1;
    }

    server.Run();
    return 0;
}
