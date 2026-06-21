/**
 * @file ServiceRegistry.h
 * @brief 服务注册表——WorldServer 注册/心跳/负载选择
 *
 * 纯 IO 进程使用共享锁（读多写少）。TCP 断线即时感知 + 30s 心跳兜底。
 */
#pragma once

#include <chrono>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "Common/Core/Types.h"

namespace MMO
{

class ServiceRegistry
{
public:
    struct ServiceInfo
    {
        std::string serviceID;
        std::string address;
        uint32      maxPlayers   = 0;
        uint32      currentPlayers = 0;
        bool        online = false;
        std::chrono::steady_clock::time_point lastHeartbeat;
    };

    // World/Social 上线注册
    void Register(const ServiceInfo& info);

    // 心跳上报（更新 currentPlayers + lastHeartbeat）
    void Heartbeat(const std::string& serviceID, uint32 currentPlayers);

    // 主动下线
    void Deregister(const std::string& serviceID);

    // TCP 断线标记离线
    void OnSocketLost(const std::string& serviceID);

    // 最少负载 World 选择（LoginServer 调用）
    const ServiceInfo* PickLeastLoadedWorld() const;

    // 检查超时 World（每 Tick 调用）
    void CheckTimeouts();

    // 获取所有在线 World 列表
    std::vector<ServiceInfo> GetOnlineServices() const;

private:
    mutable std::shared_mutex _mutex;
    std::unordered_map<std::string, ServiceInfo> _services;
    static constexpr auto kHeartbeatTimeout = std::chrono::seconds(30);
};

} // namespace MMO
