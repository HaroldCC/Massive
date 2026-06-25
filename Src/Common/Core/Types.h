/**
 * @file Types.h
 * @brief 项目级基础类型别名、编译期常量、Tracy 性能追踪宏
 */
#pragma once

#include <chrono>
#include <cstdint>

// ── 常用数值类型别名 ──────────────────────────

using int8  = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;

using uint8  = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;

// ── 编译期常量 ───────────────────────────────

/** @brief MsgID 函数表最大容量 */
inline constexpr size_t kMaxHandlers = 4096;

/** @brief 逻辑帧间隔 50ms = 20 ticks/s */
inline constexpr auto kTickInterval = std::chrono::milliseconds(50);

/** @brief MPSC 队列容量上限（防内存打爆） */
inline constexpr size_t kMaxQueueDepth = 65536;

/** @brief 无效 ID 哨兵值 */
inline constexpr uint32 kInvalidID = 0xFFFFFFFF;

// ── Tracy 性能追踪宏（零开销 —— Release 编译为空白） ──

#ifdef MASSIVE_ENABLE_TRACY
    #include <tracy/Tracy.hpp>
    #define MASSIVE_PROFILE()       ZoneScoped
    #define MASSIVE_PROFILE_NAME(x) ZoneScopedN(x)
    #define MASSIVE_FRAME_MARK()    FrameMark
#else
    #define MASSIVE_PROFILE()       (void)0
    #define MASSIVE_PROFILE_NAME(x) (void)0
    #define MASSIVE_FRAME_MARK()    (void)0
#endif
