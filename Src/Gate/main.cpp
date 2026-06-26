/**
 * @file main.cpp
 * @brief GateServer 进程入口
 *
 * 启动流程：
 *   1. Load 配置（gate.toml）
 *   2. Log::Init
 *   3. GateServer::Init（IOContextPool + TCPAcceptor + 连 World）
 *   4. GateServer::Run（ioPool.Start + 定时超时检查）
 */
#include "Gate/GateConfig.h"
#include "Gate/GateServer.h"

#include "Common/Core/Stacktrace.h"
#include "Common/Log/Log.h"

int main(int argc, char **argv)
{
    // 默认配置路径
    std::string configPath = "Config/gate.toml";
    if (argc >= 2)
    {
        configPath = argv[1];
    }

    auto cfg = MMO::GateConfig::Load(configPath);
    if (!cfg)
    {
        return 1;
    }

    MMO::Log::Init("gate", cfg->log);
    MMO::InstallStackTrace(argv[0]);

    MMO::GateServer server;
    if (!server.Init(*cfg))
    {
        MMO::Log::Error("GateServer init failed");
        return 1;
    }

    server.Run();
    MMO::Log::Info("GateServer stopped");
    return 0;
}
