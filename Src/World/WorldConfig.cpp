/**
 * @file WorldConfig.cpp
 * @brief WorldConfig Load 实现
 */

#include "World/WorldConfig.h"
#include "Common/Config/ConfigLoader.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "Common/Log/Log.h"

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

        // 常驻场景配置（MVP: 简单数组，后续扩展为 TOML table array）
        auto sceneIDs = loader.GetStringArray("world.persistent_scenes");
        for (const auto &sidStr : sceneIDs)
        {
            uint32 sceneId = static_cast<uint32>(std::stoul(sidStr));
            SceneConfig sc;
            sc.id        = sceneId;
            sc.name      = "scene_" + std::to_string(sceneId);
            sc.gridSize  = 50.0f;
            sc.viewRadiusXZ = 100.0f;
            sc.viewRadiusY  = 15.0f;
            cfg.world.persistentScenes.push_back(std::move(sc));
        }

        // 日志配置
        auto logLevelInt = loader.GetInt("log.level", 0);
        cfg.log.logDir   = loader.GetString("log.dir", "");
        if (logLevelInt >= 0 && logLevelInt <= 5)
        {
            cfg.log.level = static_cast<ELogLevel>(logLevelInt);
        }

        // LSS 密钥文件（与 LoginConfig 相同的 hex decode 逻辑）
        std::ifstream keyFile(keyPath);
        if (keyFile)
        {
            std::string hexStr;
            std::getline(keyFile, hexStr);
            keyFile.close();

            // strip whitespace
            std::string hex;
            for (char c : hexStr)
            {
                if (!std::isspace(static_cast<unsigned char>(c)))
                    hex += c;
            }

            if (hex.size() != WorldConfig::kLSSSize * 2)
            {
                Log::Error("WorldConfig: key file '{}' hex size mismatch (expected {}, got {})",
                           keyPath, WorldConfig::kLSSSize * 2, hex.size());
                return std::nullopt;
            }

            for (size_t i = 0; i < WorldConfig::kLSSSize; ++i)
            {
                auto hexVal = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                int hi = hexVal(hex[i * 2]);
                int lo = hexVal(hex[i * 2 + 1]);
                if (hi < 0 || lo < 0)
                {
                    Log::Error("WorldConfig: key file '{}' contains non-hex chars", keyPath);
                    return std::nullopt;
                }
                cfg.security.loginServerSecret[i] =
                    static_cast<uint8>((static_cast<unsigned>(hi) << 4) | static_cast<unsigned>(lo));
            }
        }

        return cfg;
    }

} // namespace MMO
