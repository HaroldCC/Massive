/**
 * @file Dump.h
 * @brief Crash Dump 编排（文本栈 + 平台 dump 编排入口）
 *
 * 本模块负责 signal-safe 的 crash 现场转储编排：
 *   1. 写 .stacktrace 文本栈（raw PC 地址，absl::GetStackTrace）
 *   2. 调 Platform::WritePlatformDump 写平台原生 dump
 *   3. _exit(1)
 *
 * 实际 OS 相关的 handler 注册 + dump 格式在 Platform_*.cpp 中。
 * CrashHandlerEntry 是 Platform_* 文件回调的入口点。
 */
#pragma once

namespace MMO
{

    /**
     * @brief 设置 crash dump 输出目录（signal-safe，仅存指针拷贝）
     *
     * 由 SetCrashDumpDirectory 转发调用。
     */
    void SetCrashDumpDirectory(const char *logDir);

    /**
     * @brief 设置进程名（用于 dump 文件命名）
     *
     * 由 InstallStackTrace 内部调用。
     */
    void SetCrashDumpProcessName(const char *name);

    /**
     * @brief Crash 内部编排入口（[[noreturn]]）
     *
     * 被 Platform_Win.cpp / Platform_Linux.cpp 的 handler 调用。
     * 1. 写 .stacktrace 文本栈
     * 2. 调 Platform::WritePlatformDump
     * 3. _exit(1)
     *
     * @param ep Windows: EXCEPTION_POINTERS* / Linux: nullptr
     */
    [[noreturn]] void CrashHandlerEntry(void *ep);

} // namespace MMO
