/**
 * @file Timestamp.h
 * @brief PostgreSQL TIMESTAMPTZ 对应的 C++ 强类型
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace MMO::DB
{

struct Timestamp
{
    int64_t unix_ms = 0;  // Unix 毫秒时间戳

    static Timestamp Now();
    static Timestamp FromUnix(int64_t ms);

    int64_t AsUnixMs() const { return unix_ms; }

    std::chrono::milliseconds ToChrono() const
    {
        return std::chrono::milliseconds(unix_ms);
    }

    bool operator==(const Timestamp&) const = default;
};

} // namespace MMO::DB
