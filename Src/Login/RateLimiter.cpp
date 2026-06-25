/**
 * @file RateLimiter.cpp
 * @brief 反暴力破解实现
 */

#include "Login/RateLimiter.h"

namespace MMO
{

    bool RateLimiter::Allow(const std::string &ip)
    {
        std::lock_guard lock(_mutex);

        auto it = _entries.find(ip);
        if (it == _entries.end())
        {
            return true;
        }

        if (it->second.failCount < kMaxFailures)
        {
            return true;
        }

        auto now = std::chrono::steady_clock::now();
        if (now > it->second.cooldownUntil)
        {
            // 冷却期已过，重置
            _entries.erase(it);
            return true;
        }

        return false;
    }

    void RateLimiter::RecordFailure(const std::string &ip)
    {
        std::lock_guard lock(_mutex);

        auto &entry = _entries[ip];
        entry.failCount++;

        if (entry.failCount >= kMaxFailures)
        {
            entry.cooldownUntil = std::chrono::steady_clock::now() + kCooldown;
        }
    }

} // namespace MMO
