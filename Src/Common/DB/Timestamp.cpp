/**
 * @file Timestamp.cpp
 * @brief Timestamp 实现
 */

#include "Common/DB/Timestamp.h"

namespace MMO::DB
{

Timestamp Timestamp::Now()
{
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return Timestamp{ms};
}

Timestamp Timestamp::FromUnix(int64_t ms)
{
    return Timestamp{ms};
}

} // namespace MMO::DB
