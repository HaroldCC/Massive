/**
 * @file GateConfig.cpp
 * @brief GateConfig Load 实现
 */
#include "Gate/GateConfig.h"
#include "Common/Config/ConfigLoader.h"

namespace MMO
{

    /**
     * @brief 从 toml 文件加载配置
     * @param path  配置路径
     * @return GateConfig，失败返回 nullopt
     */
    std::optional<GateConfig> GateConfig::Load(const std::string &path)
    {
        ConfigLoader loader;
        if (!loader.LoadFile(path))
        {
            return std::nullopt;
        }

        GateConfig cfg;

        // ── network ──
        cfg.network.port           = loader.GetUInt16("network.port", 9001);
        cfg.network.ioThreads      = loader.GetInt("network.io_threads", 8);
        cfg.network.maxConnections = loader.GetUInt32("network.max_connections", 20000);

        // ── world ──
        cfg.world.servers = loader.GetStringArray("world.servers");

        // ── heartbeat ──
        cfg.heartbeat.clientTimeoutSec = loader.GetUInt32("heartbeat.client_timeout_sec", 60);

        // ── dos ──
        cfg.dos.maxConnsPerIP = loader.GetUInt32("dos.max_conns_per_ip", 10);

        // ── log ──
        cfg.log.logDir = loader.GetString("log.dir", "");
        auto levelInt  = loader.GetInt("log.level", 0);
        if (levelInt >= 0 && levelInt <= 5)
        {
            cfg.log.level = static_cast<ELogLevel>(levelInt);
        }

        return cfg;
    }

} // namespace MMO
