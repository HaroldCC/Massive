/**
 * @file Log.cpp
 * @brief spdlog 初始化 + 内部 LogImpl 实现
 *
 * 初始化（Log::Init）：
 *   1. 控制台 sink（stdout_color_sink_mt）— 始终创建
 *   2. 如果提供 logDir，增加轮转文件 sink（rotating_file_sink_mt）
 *   3. 异步包装（block 策略：队列满时阻塞，不丢日志）
 *   4. Flush 策略：Error 及以上级别即时 flush
 *
 * LogImpl 负责拼接 traceID 前缀，spdlog pattern 输出时间/级别，函数名/行号在消息体中。
 * 之所以 traceID 不放在 spdlog pattern 中，是因为它是 thread_local 动态值，
 * spdlog 不支持每消息自定义 thread-local pattern flag——只能放入 %v（消息体）。
 */

#include "Common/Log/Log.h"

#include "Common/Core/MassiveAssert.h"

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <array>
#include <filesystem>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace MMO
{

    /**
 * @brief thread_local 默认 = kInvalidID（0xFFFFFFFF），忘记 SetTraceID 则输出 [FFFFFFFF]
 */
    thread_local uint64 Log::_traceID = kInvalidID;

    namespace
    {

        /**
         * @brief 获取可执行文件所在目录（跨平台）
         *
         * Win32:  GetModuleFileNameW -> 去掉 exe 文件名
         * Linux:  readlink("/proc/self/exe") -> 去掉 exe 文件名
         * fallback: current_path()
         */
        std::filesystem::path GetExeDir()
        {
            std::error_code ec;

#ifdef _WIN32
            wchar_t buf[MAX_PATH];
            DWORD   len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
            if (len > 0 && len < MAX_PATH)
            {
                return std::filesystem::path(buf).parent_path();
            }
#else
            char    buf[PATH_MAX];
            ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (len > 0)
            {
                buf[len] = '\0';
                return std::filesystem::path(std::string(buf)).parent_path();
            }
#endif

            return std::filesystem::current_path(ec);
        }

    } // anonymous namespace

    void Log::Init(const std::string &name, const Config &cfg)
    {
        Init(name, cfg.level, cfg.logDir);
    }

    void Log::Init(const std::string &name, ELogLevel level, const std::string_view logDir)
    {
        /**
 * @brief 初始化全局线程池（异步日志用），队列 16384，1 个工作线程
 */
        spdlog::init_thread_pool(16384, 1U);

        /**
 * @brief 收集 sink
 */
        std::vector<spdlog::sink_ptr> sinks;

        /**
 * @brief 控制台 sink（彩色输出）
 */
        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        sinks.push_back(consoleSink);

        /**
 * @brief 文件 sink（仅在指定 logDir 时添加）
 */
        if (!logDir.empty())
        {
            namespace fs = std::filesystem;
            fs::path dir = fs::path(logDir);
            if (!dir.is_absolute())
            {
                dir = GetExeDir() / dir;
            }
            fs::create_directories(dir);
            auto logPath = dir / std::format("{}.log", name);
            auto fileSink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logPath.string(),
                                                                       10 * 1024 * 1024, /**< 每个文件 10MB */
                                                                       5                 /**< 最多保留 5 个 */
                );
            sinks.push_back(fileSink);
        }

        /**
         * @brief 创建异步日志器
         *
         * block 策略：队列满时阻塞调用线程直到有空位，宁可卡一下也不丢日志。
         * 对比 overrun_oldest：MMO 线上排查依赖完整日志链，丢一条关键日志比短暂卡顿代价大得多。
         */
        auto tp     = spdlog::thread_pool();
        auto logger = std::make_shared<spdlog::async_logger>(name,
                                                             sinks.begin(),
                                                             sinks.end(),
                                                             std::move(tp),
                                                             spdlog::async_overflow_policy::block);

        /**
 * @brief 设置级别
 */
        logger->set_level(static_cast<spdlog::level::level_enum>(level));

        /**
         * @brief Pattern 格式
         *
         *   [2026-06-24 15:35:59.080] [I] [000001F3] message
         *   时间戳              | 级别 | traceID   | 消息
         */
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

        /**
 * @brief 即时 flush：Error 及以上级别立即写磁盘
 */
        logger->flush_on(spdlog::level::err);

        spdlog::set_default_logger(logger);

        // 缓存裸指针——供 ShouldLog() 快速路径使用，避免每条日志 shared_ptr refcount 开销
        LogDetail::g_rawLogger.store(logger.get(), std::memory_order_relaxed);

        // 注册断言钩子：file:line 拼入消息体，避免依赖 pattern 的 %@ 标记
        SetMassiveAssertHook([](const char *file, int line, const char *msg) {
            auto lg = spdlog::default_logger();
            if (lg)
            {
                lg->log(spdlog::level::critical, "[{}:{}] ASSERT: {}", file, line, msg);
            }
        });

        // 注册 abort 前 flush 钩子：确保异步队列中的断言消息落盘
        SetMassiveAssertFlushHook([]() {
            auto lg = spdlog::default_logger();
            if (lg)
            {
                lg->flush();
            }
        });
    }

    void Log::Shutdown()
    {
        LogDetail::g_rawLogger.store(nullptr, std::memory_order_relaxed);
        spdlog::shutdown();
    }

    void Log::Flush()
    {
        auto logger = spdlog::default_logger();
        if (logger)
        {
            logger->flush();
        }
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

        bool ShouldLog(ELogLevel level)
        {
            auto *raw = g_rawLogger.load(std::memory_order_relaxed);
            return raw && raw->should_log(static_cast<spdlog::level::level_enum>(level));
        }

        void LogImpl(ELogLevel        level,
                     std::string_view file,
                     int              line,
                     std::string_view func,
                     std::string_view message)
        {
            auto logger = spdlog::default_logger();
            if (!logger)
            {
                return;
            }

            /**
             * @brief 级别过滤已在 LogDispatch 入口（ShouldLog）完成，此处不再重复检查。
             *
             * traceID 默认 = kInvalidID（0xFFFFFFFF），忘记 SetTraceID 则输出 [FFFFFFFF]。
             */

            auto spdLevel = static_cast<spdlog::level::level_enum>(level);

            /**
             * @brief 栈缓冲区 fmt::format_to_n：短消息零 heap 分配
             *
             * 4096 字节覆盖 99% 的日志消息，超长消息 fallback 到更大的栈 buffer（8192 字节）。
             * 避免每条日志都 heap 分配 std::string。
             */
            std::array<char, 4096> stackBuf;
            auto                   result = fmt::format_to_n(stackBuf.data(),
                                                             stackBuf.size() - 1, /**< 预留 '\0' */
                                                             "[{:#010X}] [{}:{} {}] {}",
                                                             Log::GetTraceID(),
                                                             file,
                                                             line,
                                                             func,
                                                             message);

            if (result.size < stackBuf.size() - 1)
            {
                // 短消息：直接走栈 buffer，零 heap 分配
                stackBuf[static_cast<size_t>(result.size)] = '\0';
                spdlog::string_view_t payload(stackBuf.data(), result.size);
                logger->log(spdLevel, payload);
            }
            else
            {
                // 超长消息（罕见）：fallback 更大的栈 buffer
                std::array<char, 8192> largeBuf;
                auto                   largeResult              = fmt::format_to_n(largeBuf.data(),
                                                                                   largeBuf.size() - 1,
                                                                                   "[{:#010X}] [{}:{} {}] {}",
                                                                                   Log::GetTraceID(),
                                                                                   file,
                                                                                   line,
                                                                                   func,
                                                                                   message);
                largeBuf[static_cast<size_t>(largeResult.size)] = '\0';
                spdlog::string_view_t payload(largeBuf.data(), largeResult.size);
                logger->log(spdLevel, payload);
            }
        }

    } // namespace LogDetail
} // namespace MMO
