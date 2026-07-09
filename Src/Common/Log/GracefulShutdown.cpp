/**
 * @file GracefulShutdown.cpp
 * @brief 跨平台优雅关停实现
 *
 * 设计：
 *   信号处理器只设置原子标志（signal-safe）。
 *   Watcher 线程轮询该标志，检测到后调用 stopFn（停业务线程 + IO 池）。
 *   主线程从 server.Run() 返回后调用 ShutdownLog() 完成最终落盘。
 *
 * 为什么不在信号处理器中直接调用 stopFn / Log::Flush：
 *   spdlog 和业务代码都不是 signal-safe（可能持有锁）。
 *   所以走"信号 → 原子标志 → watcher 线程 → stopFn → 主线程返回 → ShutdownLog"链。
 */
#include "Common/Log/GracefulShutdown.h"

#include "Common/Log/Log.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <csignal>
    #include <unistd.h>
#endif

namespace MMO
{

    namespace
    {

        /** @brief 关停请求标志——signal handler 写入，watcher 线程读取 */
        std::atomic<bool> g_shutdownRequested {false};

        /** @brief 用户注册的 Stop 回调（watcher 线程调用） */
        std::function<void()> g_stopFn;

        /** @brief 进程名（日志输出用） */
        std::string g_processName;

#ifdef _WIN32
        /**
         * @brief Windows Ctrl+C / Ctrl+Break / Close 控制台事件处理
         *
         * 运行在单独的 handler 线程中（非信号上下文），只设置原子标志不做 I/O。
         */
        BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType)
        {
            switch (dwCtrlType)
            {
                case CTRL_C_EVENT:
                case CTRL_BREAK_EVENT:
                case CTRL_CLOSE_EVENT:
                    g_shutdownRequested.store(true, std::memory_order_release);
                    return TRUE; // 阻止默认 ExitProcess / TerminateProcess
                default:
                    return FALSE;
            }
        }
#else
        /**
         * @brief POSIX 信号处理器 (SIGINT / SIGTERM)
         *
         * signal-safe：只设置 std::atomic<bool>（lock-free），不做任何 I/O。
         */
        void SignalHandler(int sig)
        {
            (void)sig;
            g_shutdownRequested.store(true, std::memory_order_release);
        }
#endif

        /**
         * @brief CrashFlushAtexit——std::atexit 注册 Log::Flush 第二层防御
         *
         * 覆盖 main 非信号退出路径（return、exit()），确保异步日志队列排空。
         * TerminateProcess / SIGKILL 不会触发 atexit，但能覆盖绝大多数正常退出场景。
         *
         * @note 幂等性：static once_flag 确保只注册一次（被多个 server 调用也无害）。
         *       ShutdownLog 在 main 末尾显式调用，atexit 处理函数是保底——
         *       二者都会执行 flush，spdlog 可以重复 flush，没有任何副作用。
         */
        void CrashFlushAtexit()
        {
            static std::once_flag flag;
            std::call_once(flag, []() {
                std::atexit([]() {
                    // atexit 上下文：不能写 Log::Info（spdlog 可能已析构），
                    // Log::Flush 内部会做 null 检查，安全无害。
                    Log::Flush();
                });
            });
        }

    } // anonymous namespace

    void InstallGracefulShutdown(std::function<void()> stopFn, const std::string &name)
    {
        // 注册 atexit flush 保底（幂等，只生效一次）
        CrashFlushAtexit();
        g_stopFn      = std::move(stopFn);
        g_processName = name;

        g_shutdownRequested.store(false, std::memory_order_release);

        /**
         * @name 注册信号 / 控制台事件
         */
        /** @{ */
#ifdef _WIN32
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
        struct ::sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SignalHandler;
        ::sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;

        ::sigaction(SIGINT, &sa, nullptr);  // Ctrl+C
        ::sigaction(SIGTERM, &sa, nullptr); // kill
#endif
        /** @} */

        /**
         * @name 启动 watcher 线程
         *
         * watcher 每隔 100ms 轮询 g_shutdownRequested。
         * 检测到后调用 g_stopFn 停业务线程 + IO 池，main() 中的 Run() 随即返回。
         *
         * @note detach 而非 join：此线程在主线程退出时自动消亡，不会阻止进程终止。
         */
        /** @{ */
        std::thread watcher([name = g_processName]() {
            while (!g_shutdownRequested.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // 日志工作线程非信号上下文，可安全调用 Log
            Log::Info("{}: shutdown requested, stopping server...", name);

            if (g_stopFn)
            {
                g_stopFn();
            }

            Log::Info("{}: server stop callback completed", name);
        });
        watcher.detach();
        /** @} */

        Log::Info("{}: GracefulShutdown installed (Ctrl+C / SIGINT / SIGTERM)", name);
    }

    bool IsShutdownRequested()
    {
        return g_shutdownRequested.load(std::memory_order_acquire);
    }

    void ShutdownLog()
    {
        // 第一层：Flush 所有 sink——async_logger 会等待异步队列排空
        Log::Info("ShutdownLog: flushing all sinks...");
        Log::Flush();

        // 第二层：给后台工作线程一个微小的处理窗口
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // 第三层：关闭 spdlog（释放线程池、sink，内部 join 工作线程）
        Log::Shutdown();

        // 保底：stderr 输出不受 spdlog 影响
        std::cerr << "[" << g_processName << "] "
                  << "log flushed and shutdown complete" << std::endl;
    }

} // namespace MMO
