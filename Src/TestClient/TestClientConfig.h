/**
 * @file TestClientConfig.h
 * @brief TestClient CLI 参数 + TOML 配置
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "Common/Core/Types.h"

namespace MMO::TestClient
{

    /**
     * @brief TestClient 配置
     *
     * 从 TOML 文件读取，CLI 参数可覆盖部分字段。
     */
    struct TestClientConfig
    {
        // ── 服务器连接 ──
        std::string loginHost   = "127.0.0.1";
        uint16      loginPort   = 9000;

        // ── 虚拟客户端 ──
        uint32 clientCount      = 1;       // 虚拟客户端数量
        double  spawnRatePerSec = 1.0;     // 每秒创建速率
        uint32 durationSec      = 60;      // 运行时间（0 = 无限）

        // ── 行为参数 ──
        uint32 heartbeatIntervalSec = 5;   // 心跳间隔
        uint32 moveIntervalMs       = 200; // 移动间隔
        float moveSpeed             = 5.0f;// 移动速度
        float  moveRadius           = 50.0f;// 移动范围半径

        // ── 账号 ──
        std::string usernamePrefix = "testuser";  // 账号前缀，自动附加序号
        std::string password       = "test123";

        // ── 调试 ──
        bool verbose = false;  // 详细日志

        /**
         * @brief 从 TOML 文件加载配置
         * @param path  config 文件路径
         * @return TestClientConfig，加载失败返回 nullopt
         */
        static std::optional<TestClientConfig> Load(const std::string &path);
    };

} // namespace MMO::TestClient
