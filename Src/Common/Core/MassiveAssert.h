/**
 * @file MassiveAssert.h
 * @brief 自定义断言宏（双通道：stderr 保底 + 可选 Log 钩子）
 *
 * stderr 通道始终生效——零依赖，确保 Log::Init() 之前或堆损坏时也能输出。
 * 钩子通道仅在 Log::Init() 注册后生效——输出到 spdlog 控制台 + 轮转文件，
 * 携带 traceID 等上下文，方便线上排查。
 *
 * Debug:   fprintf(stderr) → 钩子(若注册) → flush 钩子(若注册) → __debugbreak() → abort()
 * Release: fprintf(stderr) → 钩子(若注册) → 继续执行
 *
 * 绝对不要用标准 assert() —— Release 下被完全移除。
 */
#pragma once

#include <cstdio>
#include <cstdlib>

// ── 断言钩子（可选注册） ──

/**
 * @brief 断言回调类型
 * @param file  源文件名（__FILE__）
 * @param line  行号
 * @param msg   断言消息
 */
using MassiveAssertHook = void (*)(const char *file, int line, const char *msg);

/** @brief 全局断言钩子（默认 nullptr，Log::Init 末尾注册） */
inline MassiveAssertHook g_massiveAssertHook = nullptr;

/** @brief abort 前 flush 钩子（默认 nullptr，Log::Init 末尾注册） */
using MassiveAssertFlushHook                           = void (*)();
inline MassiveAssertFlushHook g_massiveAssertFlushHook = nullptr;

/** @brief 注册断言钩子（Log::Init 末尾调用），传 nullptr 取消注册 */
inline void SetMassiveAssertHook(MassiveAssertHook hook)
{
    g_massiveAssertHook = hook;
}

/** @brief 注册 abort 前 flush 钩子（Log::Init 末尾调用），传 nullptr 取消注册 */
inline void SetMassiveAssertFlushHook(MassiveAssertFlushHook hook)
{
    g_massiveAssertFlushHook = hook;
}

// ── 宏定义 ──

#ifdef NDEBUG
    /** @brief Release: stderr → 钩子 → 继续执行 */
    #define MASSIVE_ASSERT(cond, msg)                                                      \
        do                                                                                 \
        {                                                                                  \
            if (!(cond))                                                                   \
            {                                                                              \
                fprintf(stderr, "[MASSIVE_ASSERT] %s:%d — %s\n", __FILE__, __LINE__, msg); \
                if (g_massiveAssertHook)                                                   \
                {                                                                          \
                    g_massiveAssertHook(__FILE__, __LINE__, msg);                          \
                }                                                                          \
            }                                                                              \
        } while (0)

    /** @brief Release: 带格式的错误记录，继续执行 */
    #define MASSIVE_ASSERT_FMT(cond, msg, ...)                                                          \
        do                                                                                              \
        {                                                                                               \
            if (!(cond))                                                                                \
            {                                                                                           \
                fprintf(stderr, "[MASSIVE_ASSERT] %s:%d — " msg "\n", __FILE__, __LINE__, __VA_ARGS__); \
                if (g_massiveAssertHook)                                                                \
                {                                                                                       \
                    char _buf[512];                                                                     \
                    snprintf(_buf, sizeof(_buf), msg, __VA_ARGS__);                                     \
                    g_massiveAssertHook(__FILE__, __LINE__, _buf);                                      \
                }                                                                                       \
            }                                                                                           \
        } while (0)

#else
    /** @brief Debug: stderr → 钩子 → flush 钩子 → debugbreak → abort */
    #define MASSIVE_ASSERT(cond, msg)                                                      \
        do                                                                                 \
        {                                                                                  \
            if (!(cond))                                                                   \
            {                                                                              \
                fprintf(stderr, "[MASSIVE_ASSERT] %s:%d — %s\n", __FILE__, __LINE__, msg); \
                if (g_massiveAssertHook)                                                   \
                {                                                                          \
                    g_massiveAssertHook(__FILE__, __LINE__, msg);                          \
                }                                                                          \
                if (g_massiveAssertFlushHook)                                              \
                {                                                                          \
                    g_massiveAssertFlushHook();                                            \
                }                                                                          \
                __debugbreak();                                                            \
                std::abort();                                                              \
            }                                                                              \
        } while (0)

    /** @brief Debug: 带格式的断言，stderr → 钩子 → flush 钩子 → debugbreak → abort */
    #define MASSIVE_ASSERT_FMT(cond, msg, ...)                                                          \
        do                                                                                              \
        {                                                                                               \
            if (!(cond))                                                                                \
            {                                                                                           \
                fprintf(stderr, "[MASSIVE_ASSERT] %s:%d — " msg "\n", __FILE__, __LINE__, __VA_ARGS__); \
                if (g_massiveAssertHook)                                                                \
                {                                                                                       \
                    char _buf[512];                                                                     \
                    snprintf(_buf, sizeof(_buf), msg, __VA_ARGS__);                                     \
                    g_massiveAssertHook(__FILE__, __LINE__, _buf);                                      \
                }                                                                                       \
                if (g_massiveAssertFlushHook)                                                           \
                {                                                                                       \
                    g_massiveAssertFlushHook();                                                         \
                }                                                                                       \
                __debugbreak();                                                                         \
                std::abort();                                                                           \
            }                                                                                           \
        } while (0)

#endif
