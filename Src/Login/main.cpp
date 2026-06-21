/**
 * @file main.cpp
 * @brief LoginServer 进程入口
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

    MMO::Log::Init("login");
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
