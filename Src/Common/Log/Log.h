/**
 * @file Log.h
 * @brief 全项目统一日志（spdlog 封装 + traceID 上下文 + source_location 自动定位）
 *
 * 核心技巧：FormatWithLocation 用 consteval 构造捕获 caller 的 source_location，
 * 不依赖模板推导——完美兼容 MSVC。
 *
 * 使用示例：
 * @code
 *   Log::Info("Player {} logged in", playerID);
 *   Log::Error("DB connect failed: {}", PQerrorMessage(conn));
 *   Log::At(ELogLevel::Warn, "script.das", 42, "unexpected value {}", val);
 * @endcode
 */
#pragma once

#include <fmt/format.h>
#include <concepts>
#include <source_location>
#include <string>
#include <string_view>

#include "Common/Core/Types.h"

namespace MMO
{

/** @brief 日志级别 */
enum class ELogLevel : uint8
{
    Trace    = 0,  // 追踪
    Debug    = 1,  // 调试
    Info     = 2,  // 信息
    Warn     = 3,  // 警告
    Error    = 4,  // 错误
    Critical = 5,  // 致命
};

namespace LogDetail
{

/** @brief 精简的 source_location（只保留 file/line/func，比 std::source_location 小） */
struct SourceLoc
{
    const char* file;
    int         line;
    const char* func;
};

/** @brief 从 std::source_location 转换为 SourceLoc */
constexpr SourceLoc FromStd(const std::source_location& loc)
{
    return {loc.file_name(), static_cast<int>(loc.line()), loc.function_name()};
}

/**
 * @brief 核心技巧：consteval 构造隐式捕获 caller 的 source_location
 *
 * 用户写 `Log::Info("msg {}", x)` 时，字符串字面量隐式转换为 FormatWithLocation，
 * 在 call site 就地捕获 source_location::current()——不依赖模板推导。
 */
struct FormatWithLocation
{
    std::string_view     fmt;
    std::source_location loc;

    template <typename T>
        requires std::convertible_to<T, std::string_view>
    consteval FormatWithLocation(
        const T&                     s,
        const std::source_location&  sl = std::source_location::current()) noexcept
        : fmt(s)
        , loc(sl)
    {
    }
};

/// @brief 日志内部实现（定义在 Log.cpp，隐藏 spdlog）
void LogImpl(ELogLevel level, const SourceLoc& loc, std::string_view message);

/**
 * @brief 分发日志：无参路径跳过格式化（零开销），有参路径 fmt::vformat
 * @tparam Args  格式化参数类型
 */
template <typename... Args>
void LogDispatch(ELogLevel level, const SourceLoc& loc, std::string_view f, Args&&... args)
{
    if constexpr (sizeof...(args) == 0)
    {
        LogImpl(level, loc, f);
    }
    else
    {
        // 项目禁用 C++ 异常（§5.4），吞掉格式化异常
        try
        {
            auto msg = fmt::vformat(f, fmt::make_format_args(args...));
            LogImpl(level, loc, msg);
        }
        catch (const fmt::format_error& e)
        {
            LogImpl(level, loc, fmt::format("[FORMAT_ERROR] {} — fmt=\"{}\"", e.what(), f));
        }
    }
}

/// @brief 带显式 file/line 的分发（供脚本层等外部调用方使用）
template <typename... Args>
void LogDispatchAt(ELogLevel level, const char* file, int line, std::string_view f, Args&&... args)
{
    SourceLoc loc{file, line, ""};
    if constexpr (sizeof...(args) == 0)
    {
        LogImpl(level, loc, f);
    }
    else
    {
        try
        {
            auto msg = fmt::vformat(f, fmt::make_format_args(args...));
            LogImpl(level, loc, msg);
        }
        catch (const fmt::format_error& e)
        {
            LogImpl(level, loc, fmt::format("[FORMAT_ERROR] {} — fmt=\"{}\"", e.what(), f));
        }
    }
}

} // namespace LogDetail

struct Log
{
    // ── 生命周期 ──

    // 初始化 spdlog（进程启动时调用一次）
    static void Init(const std::string& name = "massive", ELogLevel level = ELogLevel::Trace);

    // 关闭 spdlog
    static void Shutdown();

    // 刷新所有 sink
    static void Flush();

    // 运行时修改日志级别
    static void SetLevel(ELogLevel level);

    // ── traceID 上下文 ──

    // 设置当前线程的 traceID
    static void SetTraceID(uint64 traceID);
    // 获取当前线程的 traceID
    static uint64 GetTraceID();

    // ── 日志 API（FormatWithLocation 自动捕获 source_location）──

    template <typename... Args>
    static void Trace(LogDetail::FormatWithLocation f, Args&&... args)
    {
        LogDetail::LogDispatch(ELogLevel::Trace, LogDetail::FromStd(f.loc), f.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Debug(LogDetail::FormatWithLocation f, Args&&... args)
    {
        LogDetail::LogDispatch(ELogLevel::Debug, LogDetail::FromStd(f.loc), f.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Info(LogDetail::FormatWithLocation f, Args&&... args)
    {
        LogDetail::LogDispatch(ELogLevel::Info, LogDetail::FromStd(f.loc), f.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Warn(LogDetail::FormatWithLocation f, Args&&... args)
    {
        LogDetail::LogDispatch(ELogLevel::Warn, LogDetail::FromStd(f.loc), f.fmt, std::forward<Args>(args)...);
    }

    /**
     * @brief Error 级别日志
     * @param f   FormatWithLocation（自动捕获 source_location）
     * @param args 格式化参数
     */
    template <typename... Args>
    static void Error(LogDetail::FormatWithLocation f, Args&&... args)
    {
        LogDetail::LogDispatch(ELogLevel::Error, LogDetail::FromStd(f.loc), f.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Critical(LogDetail::FormatWithLocation f, Args&&... args)
    {
        LogDetail::LogDispatch(ELogLevel::Critical, LogDetail::FromStd(f.loc), f.fmt, std::forward<Args>(args)...);
    }

    /**
     * @brief 带显式 file/line 的日志输出
     * @param level  日志级别
     * @param file   源文件名
     * @param line   行号
     * @param fmt    格式字符串
     * @param args   格式化参数
     */
    template <typename... Args>
    static void At(ELogLevel level, const char* file, int line, std::string_view fmt, Args&&... args)
    {
        LogDetail::LogDispatchAt(level, file, line, fmt, std::forward<Args>(args)...);
    }

private:
    static thread_local uint64 _traceID;
};

} // namespace MMO
