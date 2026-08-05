/**
 * @file Timestamp.cpp
 * @brief Timestamp 实现
 */

#include "Common/DB/Timestamp.h"

#include <ctime>
#include <sstream>

#ifdef _WIN32
    #pragma warning(push)
    #pragma warning(disable : 4996) // sscanf 在 MSVC 标记为不安全，我们用的时候已确保缓冲区安全
#endif

namespace MMO::DB
{

    Timestamp Timestamp::Now()
    {
        auto now = std::chrono::system_clock::now();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        return Timestamp {ms};
    }

    Timestamp Timestamp::FromUnix(int64_t ms)
    {
        return Timestamp {ms};
    }

    Timestamp Timestamp::FromPGText(const std::string &pgText)
    {
        // PG text 格式: "2025-06-20 12:00:00[.123456][+00|+08|...]"
        // 解析为 struct tm，忽略小数秒末尾精度，提取毫秒部分
        struct tm tmBuf {};
        int       fractionalMs = 0;
        char      sign         = '+';
        int       tzHour       = 0;
        int       tzMin        = 0;

        // 解析 "YYYY-MM-DD HH:MM:SS"
        const char *p = pgText.c_str();
        auto        n = std::sscanf(p,
                                    "%d-%d-%d %d:%d:%d",
                                    &tmBuf.tm_year,
                                    &tmBuf.tm_mon,
                                    &tmBuf.tm_mday,
                                    &tmBuf.tm_hour,
                                    &tmBuf.tm_min,
                                    &tmBuf.tm_sec);
        if (n < 6)
        {
            return {};
        }
        tmBuf.tm_year -= 1900;
        tmBuf.tm_mon -= 1;
        tmBuf.tm_isdst = -1; // 让 mktime 自动判断夏令时

        // 跳过已解析的 "YYYY-MM-DD HH:MM:SS"
        while (*p && *p >= '0' && *p <= '9')
        {
            ++p;
        }
        if (*p == '-')
        {
            ++p;
            while (*p && *p >= '0' && *p <= '9')
            {
                ++p;
            }
        }
        if (*p == '-')
        {
            ++p;
            while (*p && *p >= '0' && *p <= '9')
            {
                ++p;
            }
        }
        if (*p == ' ')
        {
            ++p;
            while (*p && *p >= '0' && *p <= '9')
            {
                ++p;
            }
        }
        if (*p == ':')
        {
            ++p;
            while (*p && *p >= '0' && *p <= '9')
            {
                ++p;
            }
        }
        if (*p == ':')
        {
            ++p;
            while (*p && *p >= '0' && *p <= '9')
            {
                ++p;
            }
        }

        // 可选小数秒 ".123456"
        if (*p == '.')
        {
            ++p;
            int frac   = 0;
            int digits = 0;
            while (*p && *p >= '0' && *p <= '9' && digits < 3)
            {
                frac = frac * 10 + (*p - '0');
                ++p;
                ++digits;
            }
            // 补足到毫秒（3 位）
            while (digits < 3)
            {
                frac *= 10;
                ++digits;
            }
            fractionalMs = frac;
            // 跳过剩余小数位
            while (*p && *p >= '0' && *p <= '9')
            {
                ++p;
            }
        }

        // 可选时区偏移 "+08:00" 或 "+08"
        if (*p == '+' || *p == '-')
        {
            sign = *p;
            ++p;
            tzHour = (*p - '0') * 10 + (*(p + 1) - '0');
            p += 2;
            if (*p == ':')
            {
                ++p;
                tzMin = (*p - '0') * 10 + (*(p + 1) - '0');
            }
        }

        // 转为 time_t（UTC），然后转为 Unix ms
        auto timeUtc = std::mktime(&tmBuf);
        if (timeUtc == -1)
        {
            return {};
        }

        // 应用时区偏移（减回去：PG 的 +08 表示东八区，mktime 已按本地时区处理，
        // 但我们传给 mktime 的 tm 是 wall clock 值，mktime 会以当前时区解释。
        // 正确做法: 用 timegm 或手动修正）
        // 我们手动用 timegm 替代（Windows 上 _mkgmtime）
#ifdef _WIN32
        auto utcMs = static_cast<int64_t>(_mkgmtime(&tmBuf)) * 1000LL;
#else
        // timegm 是 POSIX 扩展，Linux/macOS 可用
        // 兼容方案: 用 mktime 然后减去本地时区偏移再应用目标时区
        // 用标准方法: 手动算 epoch
        std::tm tmCopy = tmBuf;
        auto    local  = std::mktime(&tmCopy);
        // local 是本地时区的 time_t，需要先转为 UTC
        // 用 gmtime 确定本地时区偏移
        std::tm localTm;
        localTm.tm_year = 70;
        localTm.tm_mon  = 0;
        localTm.tm_mday = 1;
        // 简单方法: 直接用系统函数
        auto utcMs = static_cast<int64_t>(timegm(&tmBuf)) * 1000LL;
#endif

        // 修正时区偏移：PG 输出时区是 UTC 偏移，文本中的值是"那时区下的本地时间"
        // 所以 utc = local - tzOffset
        int tzOffsetMinutes = tzHour * 60 + tzMin;
        if (sign == '-')
        {
            tzOffsetMinutes = -tzOffsetMinutes;
        }
        utcMs -= static_cast<int64_t>(tzOffsetMinutes) * 60LL * 1000LL;
        utcMs += fractionalMs;

        return Timestamp {utcMs};
    }

    std::string Timestamp::ToPGText() const
    {
        // 格式: "2025-06-20 12:00:00.123+00"
        auto sec = unix_ms / 1000;
        auto ms  = unix_ms % 1000;
        if (ms < 0)
        {
            ms += 1000;
            sec -= 1;
        }

        auto      tt = static_cast<time_t>(sec);
        struct tm tmBuf;
#ifdef _WIN32
        gmtime_s(&tmBuf, &tt);
#else
        gmtime_r(&tt, &tmBuf);
#endif

        std::ostringstream oss;
        oss.fill('0');
        oss << (tmBuf.tm_year + 1900) << '-' << (tmBuf.tm_mon + 1) << '-' << tmBuf.tm_mday << ' '
            << tmBuf.tm_hour << ':' << tmBuf.tm_min << ':' << tmBuf.tm_sec << '.' << ms << "+00";
        return oss.str();
    }

} // namespace MMO::DB

#ifdef _WIN32
    #pragma warning(pop)
#endif // _WIN32
