/**
 * @file GracefulShutdown.h
 * @brief 跨平台优雅关停：SIGINT/SIGTERM/Ctrl+C → 用户 Stop 回调 → Log::Flush → Log::Shutdown
 *
 * 确保关服和宕服前日志落盘的三层防御：
 *   1. GracefulShutdown：SIGINT/SIGTERM/Ctrl+C → 停业务 → 排空异步日志 → 退出
 *   2. CrashFlushAtexit：std::atexit 注册 Log::Flush，覆盖进程非信号退出路径
 *   3. Stacktrace::InstallFailureSignalHandler 已覆盖 signal-safe 的 stderr 输出
 *
 * 每个进程 main 中调用方式（以 World 为例）：
 * @code
 *   MMO::InstallGracefulShutdown([] { server.Stop(); }, "WorldServer");
 *   server.Run();
 *   MMO::ShutdownLog();  // Run 返回后确保护日志落盘
 * @endcode
 */
#pragma once

#include <functional>
#include <string>

namespace MMO
{

    /**
     * @brief 安装 SIGINT/SIGTERM 信号处理 + Windows Ctrl+C 处理
     *
     * 信号触发时（主线程上下文之外）仅设置原子标志，
     * 主线程 RunLoop 检测到标志后执行 stopFn → 回到 main → ShutdownLog。
     *
     * @param stopFn  进程自定义停服回调（Server::Stop）
     * @param name    进程名（日志输出用）
     */
    void InstallGracefulShutdown(std::function<void()> stopFn, const std::string &name);

    /**
     * @brief 查询是否收到关停信号（各进程 RunLoop 轮询用）
     * @return true  收到 SIGINT/SIGTERM/Ctrl+C
     */
    bool IsShutdownRequested();

    /**
     * @brief 最终日志落盘 + 关闭 spdlog
     *
     * 在 main() 中 server.Run() 返回后调用。
     * 内部先 Log::Flush() 确保异步队列为空，再 Log::Shutdown() 释放资源。
     */
    void ShutdownLog();

} // namespace MMO
