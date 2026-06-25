/**
 * @file ServiceRegistry.cpp
 * @brief 服务注册表实现
 */

#include "Center/ServiceRegistry.h"

namespace MMO
{

    void ServiceRegistry::Register(const ServiceInfo &info)
    {
        std::unique_lock lock(_mutex);
        auto            &entry = _services[info.serviceID];
        entry                  = info;
        entry.online           = true;
        entry.lastHeartbeat    = std::chrono::steady_clock::now();
    }

    void ServiceRegistry::Heartbeat(const std::string &serviceID, uint32 currentPlayers)
    {
        std::unique_lock lock(_mutex);
        auto             it = _services.find(serviceID);
        if (it == _services.end())
        {
            return;
        }
        it->second.currentPlayers = currentPlayers;
        it->second.lastHeartbeat  = std::chrono::steady_clock::now();
        // 心跳也是 re-online 的信号
        if (!it->second.online)
        {
            it->second.online = true;
        }
    }

    void ServiceRegistry::Deregister(const std::string &serviceID)
    {
        std::unique_lock lock(_mutex);
        auto             it = _services.find(serviceID);
        if (it == _services.end())
        {
            return;
        }
        it->second.online = false;
    }

    void ServiceRegistry::OnSocketLost(const std::string &serviceID)
    {
        std::unique_lock lock(_mutex);
        auto             it = _services.find(serviceID);
        if (it == _services.end())
        {
            return;
        }
        it->second.online = false;
    }

    const ServiceRegistry::ServiceInfo *ServiceRegistry::PickLeastLoadedWorld() const
    {
        std::shared_lock   lock(_mutex);
        const ServiceInfo *best       = nullptr;
        uint32             minPlayers = UINT32_MAX;

        for (auto &[id, info] : _services)
        {
            if (!info.online)
            {
                continue;
            }
            if (info.currentPlayers < minPlayers)
            {
                minPlayers = info.currentPlayers;
                best       = &info;
            }
        }
        return best;
    }

    void ServiceRegistry::CheckTimeouts()
    {
        auto             now = std::chrono::steady_clock::now();
        std::unique_lock lock(_mutex);
        for (auto &[id, info] : _services)
        {
            if (info.online && now - info.lastHeartbeat > kHeartbeatTimeout)
            {
                info.online = false;
            }
        }
    }

    std::vector<ServiceRegistry::ServiceInfo> ServiceRegistry::GetOnlineServices() const
    {
        std::shared_lock         lock(_mutex);
        std::vector<ServiceInfo> result;
        for (auto &[id, info] : _services)
        {
            if (info.online)
            {
                result.push_back(info);
            }
        }
        return result;
    }

} // namespace MMO
