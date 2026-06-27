/**
 * @file WorldConfig.cpp
 * @brief WorldConfig Load 实现
 */

#include "World/WorldConfig.h"
#include "Common/Config/ConfigLoader.h"

#include <cstring>
#include <fstream>

namespace MMO
{

    std::optional<WorldConfig> WorldConfig::Load(const std::string &path,
                                                 const std::string &keyPath)
    {
        ConfigLoader loader;
        if (!loader.LoadFile(path))
        {
            return std::nullopt;
        }

        WorldConfig cfg;

        cfg.network.internalPort = loader.GetUInt16("network.internal_port", 8002);
        cfg.network.ioThreads    = loader.GetInt("network.io_threads", 4);

        cfg.center.host = loader.GetString("center.host", "127.0.0.1");
        cfg.center.port = loader.GetUInt16("center.port", 7001);

        cfg.database.connString  = loader.GetString("database.conn_string", "host=127.0.0.1 port=6432 dbname=massive");
        cfg.database.workerCount = loader.GetInt("database.worker_count", 3);

        cfg.world.worldServerID = loader.GetUInt16("world.id", 1);
        cfg.world.maxPlayers    = loader.GetUInt16("world.max_players", 10000);
        cfg.world.gateAddresses = loader.GetStringArray("world.gate_addresses");

        // 日志配置
        auto logLevelInt = loader.GetInt("log.level", 0);
        cfg.log.logDir   = loader.GetString("log.dir", "");
        if (logLevelInt >= 0 && logLevelInt <= 5)
        {
            cfg.log.level = static_cast<ELogLevel>(logLevelInt);
        }

        // LSS 密钥文件
        std::ifstream keyFile(keyPath, std::ios::binary);
        if (keyFile)
        {
            keyFile.read(reinterpret_cast<char *>(cfg.security.loginServerSecret), WorldConfig::kLSSSize);
        }

        return cfg;
    }

} // namespace MMO
