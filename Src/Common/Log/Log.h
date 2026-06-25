/**
 * @file Log.h
 * @brief 全项目统一日志（spdlog 封装 + traceID 上下文 + 编译期文件名/函数名缩短）
 *
 * 核心技巧：
 *   1. FormatWithLocation 用 consteval 构造隐式捕获 caller 的 source_location，
 *      并在编译期完成文件名（仅保留文件名）和函数名（去掉返回类型和参数列表）的缩短，
 *      零运行期开销。
 *   2. 异步日志（block 策略 + ShouldLog 前置过滤），瓶颈在文件 IO 而非格式化。
 *
 * 使用示例：
 * @code
 *   Log::Info("Player {} logged in", playerID);
 *   Log::Error("DB connect failed: {}", PQerrorMessage(conn));
 *   Log::At(ELogLevel::Warn, "script.das", 42, "unexpected value {}", val);
 * @endcode
 */
#pragma once

#include <atomic>
#include <format>
#include <concepts>
#include <source_location>
#include <string>
#include <string_view>

#include "Common/Core/Types.h"

// 前向声明——避免在头文件中引入完整 spdlog 依赖
namespace spdlog
{
    class logger;
} // namespace spdlog

namespace MMO
{

    /** @brief 日志级别 */
    enum class ELogLevel : uint8
    {
        Trace    = 0, /**< 追踪 */
        Debug    = 1, /**< 调试 */
        Info     = 2, /**< 信息 */
        Warn     = 3, /**< 警告 */
        Error    = 4, /**< 错误 */
        Critical = 5, /**< 致命 */
    };

    // 仅前向声明——方便 LogDispatch 模板随后引用，无需完整定义
    struct Log;

    namespace LogDetail
    {

        /**
         * @brief 编译期缩短文件名：取路径中最后一个 '/' 或 '\' 之后的部分
         *
         *  "Src/Login/LoginServer.cpp"  ->  "LoginServer.cpp"
         *  "Common/Log/Log.h"           ->  "Log.h"
         */
        consteval std::string_view ShortenFileName(std::string_view path) noexcept
        {
            auto lastSep = std::string_view::npos;
            for (size_t i = 0; i < path.size(); ++i)
            {
                if (path[i] == '/' || path[i] == '\\')
                {
                    lastSep = i;
                }
            }
            return lastSep == std::string_view::npos ? path : path.substr(lastSep + 1);
        }

        /**
         * @brief 编译期缩短函数名：去掉返回类型、调用约定、参数列表
         *
         * MSVC 原始格式:
         *   "bool __cdecl MMO::LoginServer::Init(const struct MMO::LoginConfig &)"
         *    -> "MMO::LoginServer::Init"
         *
         * GCC/Clang 原始格式:
         *   "MMO::LoginServer::Init(MMO::LoginConfig const&)"
         *    -> "MMO::LoginServer::Init"
         *
         * 算法:
         *   1. 找到第一个 '(' 截断参数列表
         *   2. 从截断结果中找到最后一个空格，保留空格之后的内容
         */
        consteval std::string_view ShortenFunctionName(std::string_view name) noexcept
        {
            /** @brief 找第一个 '('，去掉参数列表 */
            auto parenPos = std::string_view::npos;
            for (size_t i = 0; i < name.size(); ++i)
            {
                if (name[i] == '(')
                {
                    parenPos = i;
                    break;
                }
            }
            if (parenPos == std::string_view::npos)
            {
                return name;
            }

            auto prefix = name.substr(0, parenPos);

            /** @brief 去掉末尾空白 */
            size_t end = prefix.size();
            while (end > 0 && (prefix[end - 1] == ' ' || prefix[end - 1] == '\t'))
            {
                --end;
            }
            prefix = prefix.substr(0, end);

            /** @brief 找最后一个空格，去掉返回类型和调用约定 */
            auto spacePos = std::string_view::npos;
            for (size_t i = 0; i < prefix.size(); ++i)
            {
                if (prefix[i] == ' ')
                {
                    spacePos = i;
                }
            }
            return spacePos == std::string_view::npos ? prefix : prefix.substr(spacePos + 1);
        }

        /**
         * @brief 隐式捕获 source_location + 编译期缩短 file/func
         *
         * 用户写 Log::Info("msg {}", x) 时，字符串字面量隐式转换为 FormatWithLocation，
         * consteval 构造阶段完成文件名和函数名的缩短。
         *
         * @note 缩短后的 std::string_view 指向编译器静态存储的原始字符串字面量，运行时安全。
         */
        struct FormatWithLocation
        {
            std::string_view fmt;  /**< 格式字符串 */
            std::string_view file; /**< 缩短后的文件名（仅尾部，编译期计算） */
            int              line; /**< 行号 */
            std::string_view func; /**< 缩短后的函数名（编译期计算） */

            /**
             * @brief 从字符串字面量隐式构造，捕获 caller 的 source_location
             * @param s  格式字符串
             * @param sl 调用处 source_location（由编译器填充）
             */
            template <typename T>
                requires std::convertible_to<T, std::string_view>
            consteval FormatWithLocation(
                const T                    &s,
                const std::source_location &sl = std::source_location::current()) noexcept
                : fmt(s)
                , file(ShortenFileName(sl.file_name()))
                , line(static_cast<int>(sl.line()))
                , func(ShortenFunctionName(sl.function_name()))
            {
            }
        };

        /** @brief 外部调用方传入的 source location（供 Log::At 使用） */
        struct SourceLoc
        {
            std::string_view file;
            int              line;
            std::string_view func;
        };

        /**
         * @brief 从 std::source_location 转换为 SourceLoc（不做缩短，保留原始值）
         * @param loc  source_location 对象
         * @return SourceLoc 结构体
         */
        constexpr SourceLoc FromStd(const std::source_location &loc)
        {
            return {loc.file_name(), static_cast<int>(loc.line()), loc.function_name()};
        }

        /** @brief 日志内部实现（定义在 Log.cpp，隐藏 spdlog） */
        void LogImpl(ELogLevel        level,
                     std::string_view file,
                     int              line,
                     std::string_view func,
                     std::string_view message);

        /**
         * @brief 前置级别过滤——在格式化之前判断是否需要输出（零开销快速路径）
         *
         * 与 spdlog 原生宏同级性能：读裸指针 + should_log() 一次 `mov` + 条件跳转。
         * 返回 false 时调用方无需执行 std::vformat / format_to_n。
         */
        bool ShouldLog(ELogLevel level);

        /** @brief spdlog::logger 裸指针缓存（非拥有），Init() 赋值，Shutdown() 清空 */
        inline std::atomic<spdlog::logger *> g_rawLogger = nullptr;

        /**
         * @brief 分发日志：无参路径跳过格式化（零开销），有参路径 std::vformat
         *
         * 入口处做 ShouldLog 前置过滤——级别不够直接返回，
         * 避免 std::vformat / format_to_n 在不需要输出时的无用开销。
         *
         * @tparam Args  格式化参数类型
         */
        template <typename... Args>
        void LogDispatch(ELogLevel level, const FormatWithLocation &f, Args &&...args)
        {
            if (!ShouldLog(level))
            {
                return;
            }

            if constexpr (sizeof...(args) == 0)
            {
                LogImpl(level, f.file, f.line, f.func, f.fmt);
            }
            else
            {
                /**
                 * @brief 项目禁用 C++ 异常（§5.4），吞掉格式化异常
                 *
                 * std::vformat 在格式字符串不匹配参数时抛 format_error，
                 * 捕获后输出 [FORMAT_ERROR] 标记，不传播异常。
                 */
                try
                {
                    auto msg = std::vformat(f.fmt, std::make_format_args(args...));
                    LogImpl(level, f.file, f.line, f.func, msg);
                }
                catch (const std::format_error &e)
                {
                    auto err = std::format("[FORMAT_ERROR] {} — fmt=\"{}\"", e.what(), f.fmt);
                    LogImpl(level, f.file, f.line, f.func, err);
                }
            }
        }

        /**
         * @brief 带显式 file/line 的分发（供脚本层等外部调用方使用，不做缩短）
         * @tparam Args  格式化参数类型
         */
        template <typename... Args>
        void
        LogDispatchAt(ELogLevel level, const char *file, int line, std::string_view fmtStr, Args &&...args)
        {
            if (!ShouldLog(level))
            {
                return;
            }

            SourceLoc loc {file, line, ""};
            if constexpr (sizeof...(args) == 0)
            {
                LogImpl(level, loc.file, loc.line, loc.func, fmtStr);
            }
            else
            {
                try
                {
                    auto msg = std::vformat(fmtStr, std::make_format_args(args...));
                    LogImpl(level, loc.file, loc.line, loc.func, msg);
                }
                catch (const std::format_error &e)
                {
                    auto err = std::format("[FORMAT_ERROR] {} — fmt=\"{}\"", e.what(), fmtStr);
                    LogImpl(level, loc.file, loc.line, loc.func, err);
                }
            }
        }

    } // namespace LogDetail

    struct Log
    {
        /** @brief 日志初始化参数（从 toml 配置加载） */
        struct Config
        {
            ELogLevel   level = ELogLevel::Trace; /**< 日志级别 */
            std::string logDir;                   /**< 日志目录（空=仅控制台） */
        };

        /** @name 生命周期 */

        /**
         * @brief 从 Log::Config 初始化 spdlog
         * @param name    日志器名称（进程名，如 "login"）
         * @param cfg     LogConfig（来自 toml 配置）
         */
        static void Init(const std::string &name, const Config &cfg);

        /**
         * @brief 直接指定参数初始化（兼容旧调用方）
         * @param name    日志器名称
         * @param level   日志级别
         * @param logDir  日志目录（空=仅控制台）
         */
        static void Init(const std::string     &name   = "massive",
                         ELogLevel              level  = ELogLevel::Trace,
                         const std::string_view logDir = "");

        /** @brief 关闭 spdlog */
        static void Shutdown();

        /** @brief 刷新所有 sink */
        static void Flush();

        /** @brief 运行时修改日志级别 */
        static void SetLevel(ELogLevel level);

        /**
         * @brief 前置级别过滤：格式化之前判断是否需要输出
         *
         * 转发到 LogDetail::ShouldLog，供外部调用方使用。
         * 内部日志路径（Trace/Debug/Info 等）已在 LogDispatch 入口自动调用。
         */
        static bool ShouldLog(ELogLevel level)
        {
            return LogDetail::ShouldLog(level);
        }

        /** @name traceID 上下文 */

        /** @brief 设置当前线程的 traceID */
        static void SetTraceID(uint64 traceID);
        /** @brief 获取当前线程的 traceID */
        static uint64 GetTraceID();

        /** @name 日志 API（FormatWithLocation 自动捕获 source_location） */

        template <typename... Args>
        static void Trace(LogDetail::FormatWithLocation f, Args &&...args)
        {
            LogDetail::LogDispatch(ELogLevel::Trace, f, std::forward<Args>(args)...);
        }

        template <typename... Args>
        static void Debug(LogDetail::FormatWithLocation f, Args &&...args)
        {
            LogDetail::LogDispatch(ELogLevel::Debug, f, std::forward<Args>(args)...);
        }

        template <typename... Args>
        static void Info(LogDetail::FormatWithLocation f, Args &&...args)
        {
            LogDetail::LogDispatch(ELogLevel::Info, f, std::forward<Args>(args)...);
        }

        template <typename... Args>
        static void Warn(LogDetail::FormatWithLocation f, Args &&...args)
        {
            LogDetail::LogDispatch(ELogLevel::Warn, f, std::forward<Args>(args)...);
        }

        /**
         * @brief Error 级别日志
         * @param f    FormatWithLocation（自动捕获 source_location）
         * @param args 格式化参数
         */
        template <typename... Args>
        static void Error(LogDetail::FormatWithLocation f, Args &&...args)
        {
            LogDetail::LogDispatch(ELogLevel::Error, f, std::forward<Args>(args)...);
        }

        template <typename... Args>
        static void Critical(LogDetail::FormatWithLocation f, Args &&...args)
        {
            LogDetail::LogDispatch(ELogLevel::Critical, f, std::forward<Args>(args)...);
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
        static void At(ELogLevel level, const char *file, int line, std::string_view fmt, Args &&...args)
        {
            LogDetail::LogDispatchAt(level, file, line, fmt, std::forward<Args>(args)...);
        }

    private:
        static thread_local uint64 _traceID;
    };

} // namespace MMO
