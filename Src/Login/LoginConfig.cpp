/**
 * @file LoginConfig.cpp
 * @brief LoginConfig Load 实现
 */

#include "Login/LoginConfig.h"
#include "Common/Config/ConfigLoader.h"

namespace MMO
{

std::optional<LoginConfig> LoginConfig::Load(const std::string& path)
{
    ConfigLoader loader;
    if (!loader.LoadFile(path))
    {
        return std::nullopt;
    }

    LoginConfig cfg;
    cfg.network.port          = loader.GetUInt16("network.port", 8001);
    cfg.network.ioThreads     = loader.GetInt("network.io_threads", 4);
    cfg.database.connString   = loader.GetString("database.conn_string", cfg.database.connString);
    cfg.database.workerCount  = loader.GetInt("database.worker_count", 3);
    cfg.security.loginServerSecret = loader.GetString("security.login_server_secret", "");
    cfg.world.gateIPs        = loader.GetStringArray("world.gate_ips");
    cfg.world.worldServerID  = loader.GetUInt16("world.world_server_id", 1);
    return cfg;
}

} // namespace MMO
