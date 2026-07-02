/**
 * @file MassiveAssert.h
 * @brief 自定义断言宏（stderr 保底 + 栈回溯 + Log 钩子）
 *
 * stderr 通道始终生效——零依赖，确保 Log::Init() 之前或堆损坏时也能输出。
 * 栈回溯由 DumpStackTrace 提供（Abseil backend），Log 钩子由 Log::Init 注册。
 *
 * Debug:   fprintf → 钩子(若注册) → flush → DumpStackTrace → __debugbreak() → abort()
 * Release: fprintf → DumpStackTrace → 钩子(若注册) → 继续执行
 *
 * 绝对不要用标准 assert() —— Release 下被完全移除。
 */
#pragma once

#include <cstdio>
#include <cstdlib>

// ── 栈回溯前向声明（避免 MassiveAssert.h 依赖 Stacktrace.h） ──

namespace MMO
{
    void DumpStackTrace(FILE *out, int maxFrames, int skip);
} // namespace MMO

// ── 断言钩子（可选注册） ──

using MassiveAssertHook = void (*)(const char *file, int line, const char *msg);

inline MassiveAssertHook g_massiveAssertHook = nullptr;

using MassiveAssertFlushHook                           = void (*)();
inline MassiveAssertFlushHook g_massiveAssertFlushHook = nullptr;

inline void SetMassiveAssertHook(MassiveAssertHook hook)
{
    g_massiveAssertHook = hook;
}

inline void SetMassiveAssertFlushHook(MassiveAssertFlushHook hook)
{
    g_massiveAssertFlushHook = hook;
}

// ── 宏定义 ──

#ifdef NDEBUG

    /**
     * @brief Release: stderr → 栈回溯 → 钩子 → 继续执行
     */
    #define MASSIVE_ASSERT(cond, msg)                                                      \
        do                                                                                 \
        {                                                                                  \
            if (!(cond))                                                                   \
            {                                                                              \
                fprintf(stderr, "[MASSIVE_ASSERT] %s:%d — %s\n", __FILE__, __LINE__, msg); \
                MMO::DumpStackTrace(stderr, 32, 3);                                        \
                if (g_massiveAssertHook)                                                   \
                {                                                                          \
                    g_massiveAssertHook(__FILE__, __LINE__, msg);                          \
                }                                                                          \
            }                                                                              \
        } while (0)

    /**
     * @brief Release: 带格式，stderr → 栈回溯 → 钩子 → 继续执行
     */
    #define MASSIVE_ASSERT_FMT(cond, msg, ...)                                                          \
        do                                                                                              \
        {                                                                                               \
            if (!(cond))                                                                                \
            {                                                                                           \
                fprintf(stderr, "[MASSIVE_ASSERT] %s:%d — " msg "\n", __FILE__, __LINE__, __VA_ARGS__); \
                MMO::DumpStackTrace(stderr, 32, 3);                                                     \
                if (g_massiveAssertHook)                                                                \
                {                                                                                       \
                    char _buf[512];                                                                     \
                    snprintf(_buf, sizeof(_buf), msg, __VA_ARGS__);                                     \
                    g_massiveAssertHook(__FILE__, __LINE__, _buf);                                      \
                }                                                                                       \
            }                                                                                           \
        } while (0)

#else

    /**
     * @brief Debug: fprintf → 钩子 → flush → 栈回溯 → debugbreak → abort
     *
     * Debug 下也会显式调用 DumpStackTrace——保底处理 Log::Init 之前
     * 的早期 assert（此时 FailureSignalHandler 尚未安装）。
     */
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
                MMO::DumpStackTrace(stderr, 32, 3);                                        \
                __debugbreak();                                                            \
                std::abort();                                                              \
            }                                                                              \
        } while (0)

    /**
     * @brief Debug: 带格式，fprintf → 钩子 → flush → 栈回溯 → debugbreak → abort
     */
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
                MMO::DumpStackTrace(stderr, 32, 3);                                                     \
                __debugbreak();                                                                         \
                std::abort();                                                                           \
            }                                                                                           \
        } while (0)

#endif
