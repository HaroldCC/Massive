/**
 * @file RateLimiter.h
 * @brief 反暴力破解——IP 频率限制
 *
 * 前 kMaxFailures 次错误返回错误码 + 断开；
 * 超过后在 kCooldown 时间内直接关闭连接不回应。
 */
#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

namespace MMO
{

class RateLimiter
{
public:
    // 检查是否允许（返回 false 表示被限制，应直接关闭不回应）
    bool Allow(const std::string& ip);

    // 记录一次认证失败
    void RecordFailure(const std::string& ip);

private:
    struct Entry
    {
        int failCount = 0;
        std::chrono::steady_clock::time_point cooldownUntil;
    };

    std::unordered_map<std::string, Entry> _entries;
    static constexpr int  kMaxFailures = 3;
    static constexpr auto kCooldown    = std::chrono::minutes(15);
};

} // namespace MMO
