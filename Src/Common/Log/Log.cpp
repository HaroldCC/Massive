/**
 * @file Log.cpp
 * @brief spdlog 初始化 + 内部 LogImpl 实现
 */

#include "Common/Log/Log.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

// spdlog 间接拉入了 <windows.h> → <winerror.h> 会 #define ERROR 0
#ifdef ERROR
#undef ERROR
#endif

namespace MMO
{

/** @brief thread_local 默认 = kInvalidID（0xFFFFFFFF），忘记 SetTraceID 则输出 [FFFFFFFF] */
thread_local uint64 Log::_traceID = kInvalidID;

/**
 * @brief 初始化 spdlog
 *
 * 默认 sink: stdout + colorful
 * 后续可扩展 file sink、rotating、async
 */
void Log::Initialize()
{
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("massive", std::move(sink));
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::set_default_logger(logger);
}

/** @brief 关闭 spdlog */
void Log::Shutdown()
{
    spdlog::shutdown();
}

/** @brief 设置当前线程的 traceID */
void Log::SetTraceID(uint64 traceID)
{
    _traceID = traceID;
}

/** @brief 获取当前线程的 traceID */
uint64 Log::GetTraceID()
{
    return _traceID;
}

/**
 * @brief 内部实现：拼接 traceID + file:line func + message → spdlog
 * @param level   日志级别
 * @param file    源文件名
 * @param line    行号
 * @param func    函数名
 * @param message 日志内容
 */
void Log::LogImpl(
    ELogLevel level,
    fmt::string_view file,
    int line,
    fmt::string_view func,
    std::string&& message)
{
    auto full = fmt::format(
        "[{:#010X}] [{}:{} {}] {}",
        _traceID,
        file,
        line,
        func,
        message);

    auto logger = spdlog::get("massive");
    if (!logger)
    {
        return;
    }

    switch (level)
    {
    case ELogLevel::Trace:
        logger->trace(full);
        break;
    case ELogLevel::Debug:
        logger->debug(full);
        break;
    case ELogLevel::Info:
        logger->info(full);
        break;
    case ELogLevel::Warn:
        logger->warn(full);
        break;
    case ELogLevel::Error:
        logger->error(full);
        break;
    case ELogLevel::Critical:
        logger->critical(full);
        break;
    }
}

} // namespace MMO
