/**
 * @file GateConfig.h
 * @brief GateServer 专属强类型配置
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Common/Core/Types.h"
#include "Common/Log/Log.h"

namespace MMO
{

    struct GateConfig
    {
        struct Network
        {
            uint16 port           = 9001;  // 客户端监听端口
            int    ioThreads      = 8;     // IO 线程数（Gate 纯 IO，可多配）
            uint32 maxConnections = 20000; // 全局连接上限
        } network;

        struct World
        {
            std::vector<std::string> servers; // WorldServer 内网地址列表
        } world;

        struct Heartbeat
        {
            uint32 clientTimeoutSec = 60; // 客户端无心跳超时断连
        } heartbeat;

        struct DoS
        {
            uint32 maxConnsPerIP = 10; // 单 IP 最大连接数
        } dos;

        /**
         * @brief 日志配置
         */
        Log::Config log;

        /**
         * @brief 从 toml 文件加载
         * @param path  配置路径
         * @return GateConfig，失败返回 nullopt
         */
        static std::optional<GateConfig> Load(const std::string &path);
    };

} // namespace MMO
