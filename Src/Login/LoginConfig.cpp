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
#include "Common/Crypto/Hex.h"
#include "Common/Log/Log.h"

#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

namespace MMO
{

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
            if (!Crypto::HexDecode(lssHex, cfg.security.loginServerSecret, LoginConfig::kLSSSize))
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
                // 自动生成 32B 随机密钥并写入文件
                Log::Warn("LoginConfig: key file '{}' not found, generating random key", keyPath);
                std::string newHex;
                newHex.reserve(LoginConfig::kLSSSize * 2);
                std::random_device rd;
                for (size_t i = 0; i < LoginConfig::kLSSSize; ++i)
                {
                    uint8_t     byte      = static_cast<uint8_t>(rd());
                    const char *hexDigits = "0123456789abcdef";
                    newHex += hexDigits[byte >> 4];
                    newHex += hexDigits[byte & 0x0f];
                }
                std::ofstream keyOut(keyPath);
                if (!keyOut)
                {
                    Log::Error("LoginConfig: failed to write generated key to '{}'", keyPath);
                    return std::nullopt;
                }
                keyOut << newHex;
                keyOut.close();
                fileContent = newHex;
                Log::Info("LoginConfig: generated new LSS key -> '{}'", keyPath);
            }
            std::string hex = Crypto::StripWhitespace(fileContent);
            if (!Crypto::HexDecode(hex, cfg.security.loginServerSecret, LoginConfig::kLSSSize))
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
