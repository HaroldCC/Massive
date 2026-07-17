/**
 * @file WorldConfig.h
 * @brief WorldServer 专属强类型配置
 */
#pragma once

#include "World/SceneManager.h" // SceneConfig

namespace MMO
{

    struct WorldConfig
    {
        static constexpr size_t kLSSSize = 32; // LoginServerSecret（与 LoginServer 共享）

        struct Network
        {
            uint16 internalPort = 8002; // 内网监听端口（Gate 连接用）
            int    ioThreads    = 4;
        } network;

        struct Center
        {
            std::string host = "127.0.0.1";
            uint16      port = 7001;
        } center;

        struct Database
        {
            std::string connString  = "host=127.0.0.1 port=6432 dbname=massive";
            int         workerCount = 3;
        } database;

        struct Security
        {
            uint8 loginServerSecret[kLSSSize] = {};
        } security;

        struct WorldInfo
        {
            uint16                   worldServerID = 1;
            uint16                   maxPlayers    = 10000;
            std::vector<std::string> gateAddresses;    // Gate 内网地址列表
            std::vector<SceneConfig> persistentScenes; // 常驻场景配置
        } world;

        struct Script
        {
            std::string dasRoot = "Script"; // das 生态根目录（daslib/Script/System 等）
        } script;

        Log::Config log;

        static std::optional<WorldConfig> Load(const std::string &path,
                                               const std::string &keyPath = "Config/login.key");
    };

} // namespace MMO
