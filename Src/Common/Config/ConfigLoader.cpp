/**
 * @file ConfigLoader.cpp
 * @brief ConfigLoader 实现（唯一直接 include toml++ 的翻译单元）
 */

#include "Common/Config/ConfigLoader.h"

#include <toml.hpp>

namespace MMO
{

    ConfigLoader::ConfigLoader() : _table(new toml::table {})
    {
    }

    ConfigLoader::~ConfigLoader()
    {
        delete static_cast<toml::table *>(_table);
    }

    ConfigLoader::ConfigLoader(ConfigLoader &&other) noexcept : _table(other._table)
    {
        other._table = nullptr;
    }

    ConfigLoader &ConfigLoader::operator=(ConfigLoader &&other) noexcept
    {
        if (this != &other)
        {
            delete static_cast<toml::table *>(_table);
            _table       = other._table;
            other._table = nullptr;
        }
        return *this;
    }

    bool ConfigLoader::LoadFile(const std::string &path)
    {
        try
        {
            auto &tbl = *static_cast<toml::table *>(_table);
            tbl       = toml::parse_file(path);
            return true;
        }
        catch (const toml::parse_error &)
        {
            return false;
        }
    }

    int32 ConfigLoader::GetInt(const std::string &path, int32 defaultVal) const
    {
        auto &tbl = *static_cast<toml::table *>(_table);
        return tbl.at_path(path).value_or(defaultVal);
    }

    uint16 ConfigLoader::GetUInt16(const std::string &path, uint16 defaultVal) const
    {
        auto &tbl  = *static_cast<toml::table *>(_table);
        auto  node = tbl.at_path(path);
        if (!node)
        {
            return defaultVal;
        }
        if (auto val = node.value<int64_t>())
        {
            return static_cast<uint16>(*val);
        }
        return defaultVal;
    }

    uint32 ConfigLoader::GetUInt32(const std::string &path, uint32 defaultVal) const
    {
        auto &tbl  = *static_cast<toml::table *>(_table);
        auto  node = tbl.at_path(path);
        if (!node)
        {
            return defaultVal;
        }
        if (auto val = node.value<int64_t>())
        {
            return static_cast<uint32>(*val);
        }
        return defaultVal;
    }

    std::string ConfigLoader::GetString(const std::string &path, const std::string &defaultVal) const
    {
        auto &tbl = *static_cast<toml::table *>(_table);
        return tbl.at_path(path).value_or(defaultVal);
    }

    bool ConfigLoader::GetBool(const std::string &path, bool defaultVal) const
    {
        auto &tbl = *static_cast<toml::table *>(_table);
        return tbl.at_path(path).value_or(defaultVal);
    }

    std::vector<std::string> ConfigLoader::GetStringArray(const std::string &path) const
    {
        std::vector<std::string> result;
        auto                    &tbl  = *static_cast<toml::table *>(_table);
        auto                     node = tbl.at_path(path);
        if (!node || !node.is_array())
        {
            return result;
        }

        for (auto &elem : *node.as_array())
        {
            if (auto val = elem.value<std::string>())
            {
                result.push_back(*val);
            }
            else if (auto val = elem.value<int64_t>())
            {
                result.push_back(std::to_string(*val));
            }
        }
        return result;
    }

} // namespace MMO
