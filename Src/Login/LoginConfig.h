/**
 * @file LoginConfig.h
 * @brief LoginServer 专属强类型配置
 */
#pragma once

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "Common/Core/Types.h"
#include "Common/Log/Log.h"

namespace MMO
{

struct LoginConfig
{
    static constexpr size_t kLSSSize = 32;  ///< LoginServerSecret 长度（AES-256 key）

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
        uint8 loginServerSecret[kLSSSize] = {};  ///< 32B LSS
    } security;

    struct World
    {
        std::vector<std::string> gateIPs;
        uint16 worldServerID = 1;
    } world;

    /// @brief 日志配置（Log::Init 入参）
    Log::Config log;

    /**
     * @brief 从 toml 文件加载
     * @param path       配置路径
     * @param keyPath    LSS 密钥文件路径（toml 中为空时 fallback）
     * @return LoginConfig，失败返回 nullopt
     */
    static std::optional<LoginConfig> Load(const std::string& path,
                                           const std::string& keyPath = "Config/login.key");
};

} // namespace MMO
