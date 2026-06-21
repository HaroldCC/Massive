/**
 * @file LoginConfig.h
 * @brief LoginServer 专属强类型配置
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/Core/Types.h"

namespace MMO
{

struct LoginConfig
{
    struct Network
    {
        uint16 port      = 8001;
        int    ioThreads = 4;
    } network;

    struct Database
    {
        std::string connString  = "host=127.0.0.1 port=6432 dbname=massive";
        int         workerCount = 3;
    } database;

    struct Security
    {
        std::string loginServerSecret;
    } security;

    struct World
    {
        std::vector<std::string> gateIPs;
        uint16 worldServerID = 1;
    } world;

    /**
     * @brief 从 toml 文件加载
     * @param path  配置路径
     * @return LoginConfig，失败返回 nullopt
     */
    static std::optional<LoginConfig> Load(const std::string& path);
};

} // namespace MMO
