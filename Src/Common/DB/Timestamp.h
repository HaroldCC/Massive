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
        int64_t unix_ms = 0; // Unix 毫秒时间戳

        static Timestamp Now();
        static Timestamp FromUnix(int64_t ms);

        int64_t AsUnixMs() const
        {
            return unix_ms;
        }

        std::chrono::milliseconds ToChrono() const
        {
            return std::chrono::milliseconds(unix_ms);
        }

        /**
         * @brief 从 PostgreSQL TIMESTAMPTZ text 格式解析
         *
         * PG text 格式示例: "2025-06-20 12:00:00+00" / "2025-06-20 12:00:00.123+08"
         * 仅支持 UTC 偏移（+00/+08 等），不解析时区名（America/New_York）
         */
        static Timestamp FromPGText(const std::string &pgText);

        /**
         * @brief 格式化为 PG TIMESTAMPTZ text（UTC，含毫秒）
         * 如 "2025-06-20 12:00:00.123+00"
         */
        std::string ToPGText() const;

        bool operator==(const Timestamp &) const = default;
        bool operator<(const Timestamp &other) const { return unix_ms < other.unix_ms; }
    };

} // namespace MMO::DB
