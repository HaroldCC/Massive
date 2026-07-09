/**
 * @file Stacktrace.h
 * @brief 栈回溯与 Crash Handler 高层接口
 *
 * 使用方式（每个进程 main 中调用一次）:
 * @code
 *   #include "Common/Core/Stacktrace.h"
 *   #include "Common/Core/Dump.h"  // SetCrashDumpDirectory
 *   int main(int argc, char **argv) {
 *       MMO::InstallStackTrace(argv[0]);  // 必须在 Log::Init 之后
 *       MMO::SetCrashDumpDirectory(cfg->log.logDir.c_str());
 *       // ...
 *   }
 * @endcode
 *
 * 架构：
 *   Stacktrace.h    → 高层接口（InstallStackTrace, DumpStackTrace）
 *   Dump.h          → crash 编排（文本栈 + 平台 dump + _exit）
 *   Platform.h      → 平台抽象声明
 *   Platform_Win.cpp → Windows 实现（MiniDumpWriteDump, SEH）
 *   Platform_Linux.cpp → Linux 实现（fork+abort, sigaction）
 */
#pragma once

#include <cstdio>

namespace MMO
{

    /**
     * @brief 打印当前线程栈回溯到 FILE（符号化版本，非 signal-safe）
     *
     * 线程安全（调用 absl::Symbolize，可能 heap 分配），
     * 用于 MASSIVE_ASSERT Release 路径（进程不终止）。
     *
     * @param out       输出 FILE (默认 stderr)
     * @param maxFrames 最大帧数 (默认 32)
     * @param skip      跳过的栈帧数 (默认 2)
     */
    void DumpStackTrace(FILE *out = stderr, int maxFrames = 32, int skip = 2);

    /**
     * @brief 安装 Crash dump 处理器 + 初始化符号解析
     *
     * 必须在 Log::Init() 之后调用。
     *
     * 安装内容:
     *   - absl InitializeSymbolizer（供 DumpStackTrace 符号化）
     *   - 平台原生 crash dump handler（Dump.h + Platform.h）
     *
     * @param argv0 程序路径（传递给 absl::InitializeSymbolizer）
     */
    void InstallStackTrace(const char *argv0);

} // namespace MMO
