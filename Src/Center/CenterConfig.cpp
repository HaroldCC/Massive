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
    return cfg;
}

} // namespace MMO
