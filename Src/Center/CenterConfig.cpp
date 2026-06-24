/**
 * @file CenterConfig.cpp
 * @brief CenterConfig Load 实现
 */

#include "Center/CenterConfig.h"
#include "Common/Config/ConfigLoader.h"

namespace MMO
{

std::optional<CenterConfig> CenterConfig::Load(const std::string& path)
{
    ConfigLoader loader;
    if (!loader.LoadFile(path))
    {
        return std::nullopt;
    }

    CenterConfig cfg;
    cfg.network.port      = loader.GetUInt16("network.port", 9000);
    cfg.network.ioThreads = loader.GetInt("network.io_threads", 2);

    /** @brief 日志配置 */
    cfg.log.logDir = loader.GetString("log.dir", "");
    auto logLevelInt = loader.GetInt("log.level", 0);
    if (logLevelInt >= 0 && logLevelInt <= 5)
    {
        cfg.log.level = static_cast<ELogLevel>(logLevelInt);
    }

    return cfg;
}

} // namespace MMO
