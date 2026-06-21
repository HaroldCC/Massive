/**
 * @file CenterConfig.h
 * @brief CenterServer 专属强类型配置
 */
#pragma once

#include <optional>
#include <string>

#include "Common/Core/Types.h"

namespace MMO
{

struct CenterConfig
{
    struct Network
    {
        uint16 port      = 9000;
        int    ioThreads = 2;
    } network;

    static std::optional<CenterConfig> Load(const std::string& path);
};

} // namespace MMO
