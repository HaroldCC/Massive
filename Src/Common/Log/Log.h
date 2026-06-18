/**
 * @file Log.h
 * @brief 全项目统一日志（spdlog 封装 + traceID 上下文 + source_location 自动定位）
 */
#pragma once

#include <fmt/format.h>
#include <source_location>

#include "Common/Core/Types.h"

namespace MMO
{

/**
 * @brief 日志级别
 */
enum class ELogLevel : uint8
{
    Trace    = 0, ///< 追踪
    Debug    = 1, ///< 调试
    Info     = 2, ///< 信息
    Warn     = 3, ///< 警告
    Error    = 4, ///< 错误
    Critical = 5, ///< 致命
};

/**
 * @brief 全项目统一日志入口
 *
 * 全静态方法，非实例化。
 * 自动捕获 source_location (file/line/func) 并携带 thread_local traceID。
 *
 * 使用示例：
 * @code
 *   Log::SetTraceID(traceID);
 *   Log::Info("Scene {} loaded {} entities", sceneID, count);
 * @endcode
 */
class Log
{
public:
    static void Initialize();
    static void Shutdown();

    // ── traceID 上下文（thread_local，调用整条链自动携带）──

    static void SetTraceID(uint64 traceID);
    static uint64 GetTraceID();

    // ── 日志 API（source_location 自动捕获 file/line/func）──

    /** @brief Trace 级别日志 */
    template <typename... Args>
    static void Trace(
        fmt::format_string<Args...> fmt,
        Args&&... args,
        std::source_location loc = std::source_location::current())
    {
        LogImpl(ELogLevel::Trace, loc.file_name(), loc.line(), loc.function_name(),
            fmt::vformat(fmt, fmt::make_format_args(args...)));
    }

    /** @brief Debug 级别日志 */
    template <typename... Args>
    static void Debug(
        fmt::format_string<Args...> fmt,
        Args&&... args,
        std::source_location loc = std::source_location::current())
    {
        LogImpl(ELogLevel::Debug, loc.file_name(), loc.line(), loc.function_name(),
            fmt::vformat(fmt, fmt::make_format_args(args...)));
    }

    /** @brief Info 级别日志 */
    template <typename... Args>
    static void Info(
        fmt::format_string<Args...> fmt,
        Args&&... args,
        std::source_location loc = std::source_location::current())
    {
        LogImpl(ELogLevel::Info, loc.file_name(), loc.line(), loc.function_name(),
            fmt::vformat(fmt, fmt::make_format_args(args...)));
    }

    /** @brief Warn 级别日志 */
    template <typename... Args>
    static void Warn(
        fmt::format_string<Args...> fmt,
        Args&&... args,
        std::source_location loc = std::source_location::current())
    {
        LogImpl(ELogLevel::Warn, loc.file_name(), loc.line(), loc.function_name(),
            fmt::vformat(fmt, fmt::make_format_args(args...)));
    }

    /** @brief Error 级别日志 */
    template <typename... Args>
    static void Error(
        fmt::format_string<Args...> fmt,
        Args&&... args,
        std::source_location loc = std::source_location::current())
    {
        LogImpl(ELogLevel::Error, loc.file_name(), loc.line(), loc.function_name(),
            fmt::vformat(fmt, fmt::make_format_args(args...)));
    }

    /** @brief Critical 级别日志 */
    template <typename... Args>
    static void Critical(
        fmt::format_string<Args...> fmt,
        Args&&... args,
        std::source_location loc = std::source_location::current())
    {
        LogImpl(ELogLevel::Critical, loc.file_name(), loc.line(), loc.function_name(),
            fmt::vformat(fmt, fmt::make_format_args(args...)));
    }

private:
    static void LogImpl(
        ELogLevel level,
        fmt::string_view file,
        int line,
        fmt::string_view func,
        std::string&& message);

    static thread_local uint64 _traceID;
};

} // namespace MMO
