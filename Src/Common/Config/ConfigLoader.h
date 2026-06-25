/**
 * @file ConfigLoader.h
 * @brief TOML 配置加载工具（pImpl 隐藏 toml++，业务代码不耦合解析库）
 *
 * 各进程定义自己的强类型 Config 结构体，在 Load 中调用本工具填充值。
 *
 * 使用示例：
 * @code
 *   ConfigLoader loader;
 *   loader.LoadFile("Config/login.toml");
 *   auto port = loader.GetUInt16("network.port", 8001);
 * @endcode
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Common/Core/Types.h"

namespace MMO
{

    class ConfigLoader
    {
    public:
        ConfigLoader();
        ~ConfigLoader();

        ConfigLoader(const ConfigLoader &)            = delete;
        ConfigLoader &operator=(const ConfigLoader &) = delete;
        ConfigLoader(ConfigLoader &&) noexcept;
        ConfigLoader &operator=(ConfigLoader &&) noexcept;

        /**
         * @brief 加载 toml 文件
         * @param path  文件路径
         * @return 成功返回 true；文件不存在/语法错误返回 false
         */
        bool LoadFile(const std::string &path);

        // ── 类型安全取值（path 用点分，如 "network.port"；缺失返回 default）──

        int         GetInt(const std::string &path, int defaultVal) const;
        uint16      GetUInt16(const std::string &path, uint16 defaultVal) const;
        uint32      GetUInt32(const std::string &path, uint32 defaultVal) const;
        std::string GetString(const std::string &path, const std::string &defaultVal) const;
        bool        GetBool(const std::string &path, bool defaultVal) const;

        /**
         * @brief 取字符串数组（TOML array of strings）
         * @param path  配置路径
         * @return 字符串向量，路径不存在/类型不对返回空 vector
         */
        std::vector<std::string> GetStringArray(const std::string &path) const;

    private:
        void *_table = nullptr; // pImpl: toml::table*，隐藏 toml++ 依赖
    };

} // namespace MMO
