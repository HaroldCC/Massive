/**
 * @file CenterConfig.h
 * @brief CenterServer 专属强类型配置
 */
#pragma once

#include <optional>
#include <string>

#include "Common/Core/Types.h"
#include "Common/Log/Log.h"

namespace MMO
{

    struct CenterConfig
    {
        struct Network
        {
            uint16 port      = 9000;
            int    ioThreads = 2;
        } network;

        /**
         * @brief 日志配置（Log::Init 入参）
         */
        Log::Config log;

        /**
         * @brief 从 toml 文件加载配置
         * @param path 配置路径
         * @return CenterConfig，失败返回 nullopt
         */
        static std::optional<CenterConfig> Load(const std::string &path);
    };

} // namespace MMO
