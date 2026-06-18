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

void Log::Init(const std::string& name, ELogLevel level)
{
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>(name, std::move(sink));
    logger->set_level(static_cast<spdlog::level::level_enum>(level));
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::set_default_logger(logger);
}

void Log::Shutdown()
{
    spdlog::shutdown();
}

void Log::Flush()
{
    spdlog::default_logger()->flush();
}

void Log::SetLevel(ELogLevel level)
{
    auto logger = spdlog::default_logger();
    if (logger)
    {
        logger->set_level(static_cast<spdlog::level::level_enum>(level));
    }
}

void Log::SetTraceID(uint64 traceID)
{
    _traceID = traceID;
}

uint64 Log::GetTraceID()
{
    return _traceID;
}

namespace LogDetail
{

void LogImpl(ELogLevel level, const SourceLoc& loc, std::string_view message)
{
    auto full = fmt::format(
        "[{:#010X}] [{}:{} {}] {}",
        Log::GetTraceID(),
        loc.file,
        loc.line,
        loc.func,
        message);

    auto logger = spdlog::default_logger();
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

} // namespace LogDetail
} // namespace MMO
