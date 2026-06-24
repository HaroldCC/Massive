/**
 * @file main.cpp
 * @brief CenterServer 进程入口
 */

#include "Center/CenterConfig.h"
#include "Center/CenterServer.h"
#include "Common/Log/Log.h"

int main()
{
    auto cfg = MMO::CenterConfig::Load("Config/center.toml");
    if (!cfg)
    {
        return 1;
    }

    MMO::Log::Init("center", cfg->log);
    MMO::CenterServer server;
    if (!server.Init(*cfg))
    {
        MMO::Log::Error("CenterServer init failed");
        return 1;
    }

    server.Run();
    MMO::Log::Info("CenterServer stopped");
    return 0;
}
