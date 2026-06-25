/**
 * @file LoginConfig.cpp
 * @brief LoginConfig Load 实现
 *
 * LSS 加载优先级：
 *   1. toml 中 security.login_server_secret 非空 → hex decode → 32B
 *   2. toml 中为空 → 读取 keyPath 密钥文件 → strip whitespace → hex decode → 32B
 *   3. 两者均失败/长度为 0 → 报错 + nullopt
 */
#include "Login/LoginConfig.h"
#include "Common/Config/ConfigLoader.h"
#include "Common/Log/Log.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace MMO
{

    // hex 字符 → 4-bit 值，非法字符返回 -1
    static int HexValue(char c)
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return -1;
    }

    // hex 字符串 → 32B LSS，失败返回 false
    static bool DecodeLSS(const std::string &hex, uint8 (&lss)[LoginConfig::kLSSSize])
    {
        if (hex.size() != LoginConfig::kLSSSize * 2)
        {
            return false;
        }
        for (size_t i = 0; i < LoginConfig::kLSSSize; ++i)
        {
            int hi = HexValue(hex[i * 2]);
            int lo = HexValue(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0)
            {
                return false;
            }
            lss[i] = static_cast<uint8>((static_cast<unsigned>(hi) << 4) | static_cast<unsigned>(lo));
        }
        return true;
    }

    // 读取文件全部内容为 string
    static std::string ReadFile(const std::string &path)
    {
        std::ifstream f(path);
        if (!f.is_open())
        {
            return {};
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // 移除字符串中的空白字符（空格/tab/换行/回车）
    static std::string StripWhitespace(const std::string &s)
    {
        std::string result;
        result.reserve(s.size());
        for (char c : s)
        {
            if (!std::isspace(static_cast<unsigned char>(c)))
            {
                result += c;
            }
        }
        return result;
    }

    std::optional<LoginConfig> LoginConfig::Load(const std::string &path, const std::string &keyPath)
    {
        ConfigLoader loader;
        if (!loader.LoadFile(path))
        {
            return std::nullopt;
        }

        LoginConfig cfg;
        cfg.network.port         = loader.GetUInt16("network.port", 8001);
        cfg.network.ioThreads    = loader.GetInt("network.io_threads", 4);
        cfg.database.connString  = loader.GetString("database.conn_string", cfg.database.connString);
        cfg.database.workerCount = loader.GetInt("database.worker_count", 3);
        cfg.world.gateIPs        = loader.GetStringArray("world.gate_ips");
        cfg.world.worldServerID  = loader.GetUInt16("world.world_server_id", 1);

        // ── 日志配置 ──
        cfg.log.logDir   = loader.GetString("log.dir", "");
        auto logLevelInt = loader.GetInt("log.level", 0);
        // 钳位到有效枚举范围
        if (logLevelInt >= 0 && logLevelInt <= 5)
        {
            cfg.log.level = static_cast<ELogLevel>(logLevelInt);
        }

        // ── LSS 加载 ──
        std::string lssHex = loader.GetString("security.login_server_secret", "");
        if (!lssHex.empty())
        {
            if (!DecodeLSS(lssHex, cfg.security.loginServerSecret))
            {
                Log::Error("LoginConfig: security.login_server_secret hex decode failed "
                           "(expected {} hex chars)",
                           LoginConfig::kLSSSize * 2);
                return std::nullopt;
            }
        }
        else
        {
            std::string fileContent = ReadFile(keyPath);
            if (fileContent.empty())
            {
                Log::Error("LoginConfig: LSS not found (toml empty, key file '{}' missing or empty)",
                           keyPath);
                return std::nullopt;
            }
            std::string hex = StripWhitespace(fileContent);
            if (!DecodeLSS(hex, cfg.security.loginServerSecret))
            {
                Log::Error("LoginConfig: key file '{}' hex decode failed "
                           "(expected {} hex chars, got {})",
                           keyPath,
                           LoginConfig::kLSSSize * 2,
                           hex.size());
                return std::nullopt;
            }
        }

        return cfg;
    }

} // namespace MMO
