/**
 * @file TestClientConfig.cpp
 * @brief TestClientConfig 实现——TOML 解析
 */

#include "TestClient/TestClientConfig.h"
#include "Common/Log/Log.h"

#include <toml.hpp>

namespace MMO::TestClient
{

    std::optional<TestClientConfig> TestClientConfig::Load(const std::string &path)
    {
        try
        {
            auto tbl = toml::parse_file(path);

            TestClientConfig cfg;

            // [server]
            if (auto server = tbl["server"].as_table())
            {
                cfg.loginHost = server->get("host")->value_or("127.0.0.1");
                cfg.loginPort = static_cast<uint16>(server->get("port")->value_or(9000));
            }

            // [clients]
            if (auto clients = tbl["clients"].as_table())
            {
                cfg.clientCount     = static_cast<uint32>(clients->get("count")->value_or(1));
                cfg.spawnRatePerSec = clients->get("spawn_rate_per_sec")->value_or(1.0);
                cfg.durationSec     = static_cast<uint32>(clients->get("duration_sec")->value_or(60));
            }

            // [behavior]
            if (auto behavior = tbl["behavior"].as_table())
            {
                cfg.heartbeatIntervalSec =
                    static_cast<uint32>(behavior->get("heartbeat_interval_sec")->value_or(5));
                cfg.moveIntervalMs = static_cast<uint32>(behavior->get("move_interval_ms")->value_or(200));
                cfg.moveSpeed      = static_cast<float>(behavior->get("move_speed")->value_or(5.0));
                cfg.moveRadius     = static_cast<float>(behavior->get("move_radius")->value_or(50.0f));
            }

            // [account]
            if (auto account = tbl["account"].as_table())
            {
                cfg.usernamePrefix = account->get("username_prefix")->value_or("testuser");
                cfg.password       = account->get("password")->value_or("test123");
            }

            // [debug]
            cfg.verbose = tbl["debug"]["verbose"].value_or(false);

            Log::Info("TestClientConfig: loaded from {} ({} clients, {}s)",
                      path,
                      cfg.clientCount,
                      cfg.durationSec);
            return cfg;
        }
        catch (const toml::parse_error &e)
        {
            Log::Error("TestClientConfig: parse error: {}", e.what());
            return std::nullopt;
        }
        catch (const std::exception &e)
        {
            Log::Error("TestClientConfig: error: {}", e.what());
            return std::nullopt;
        }
    }

} // namespace MMO::TestClient
