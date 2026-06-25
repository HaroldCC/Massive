/**
 * @file Stacktrace.h
 * @brief 栈回溯与 Crash Handler (基于 Abseil debugging)
 *
 * 使用方式 (每个进程 main 中调用一次):
 * @code
 *   #include "Common/Core/Stacktrace.h"
 *   int main(int argc, char **argv) {
 *       MMO::InstallStackTrace(argv[0]);  // 必须在 Log::Init 之后
 *       // ... 业务代码 ...
 *   }
 * @endcode
 *
 * 两层防御:
 *   1. FailureSignalHandler - SIGSEGV/SIGABRT/SIGILL/... -> stderr 打印完整栈 -> abort
 *   2. MASSIVE_ASSERT 宏内 - Release 下输出栈到 stderr + 日志文件 (Abseil backend)
 *
 * 依赖: absl/debugging/stacktrace.h + symbolize.h + failure_signal_handler.h
 *   - 随 abseil 目标打包
 */
#pragma once

#include <cstdio>

namespace MMO
{

    /**
     * @brief 打印当前线程栈回溯到 FILE (最多 maxFrames 帧)
     *
     * 线程安全、非 signal-safe (调用 absl::Symbolize, 可能 heap 分配),
     * 用于 MASSIVE_ASSERT Release 路径 (进程不终止、可安全使用).
     *
     * @param out       输出 FILE (stderr / 日志文件)
     * @param maxFrames 最大帧数 (默认 32)
     * @param skip      跳过的栈帧数 (默认 2: 跳过自身 + 宏包装层)
     */
    void DumpStackTrace(FILE *out = stderr, int maxFrames = 32, int skip = 2);

    /**
     * @brief 安装 Crash 信号处理器 + 初始化符号解析
     *
     * 必须在 Log::Init() 之后调用 (信号处理器可能输出到日志文件).
     * 覆盖信号: SIGSEGV, SIGILL, SIGFPE, SIGABRT, SIGBUS, SIGTRAP
     *
     * Signal handler 中仅做 signal-safe 操作:
     *   - absl::GetStackTrace(addr only, no Symbolize)
     *   - write() to STDERR_FILENO
     *   - _exit(1)
     *
     * @param argv0 程序路径 (传递给 absl::InitializeSymbolizer)
     */
    void InstallStackTrace(const char *argv0);

} // namespace MMO
