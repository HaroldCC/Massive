/**
 * @file Platform.h
 * @brief 全项目统一的平台检测宏 + 平台抽象函数声明
 *
 * 使用方式：
 *   1. 任何需要平台检测的地方 #include "Common/Core/Platform.h"
 *      然后使用 MMO_PLATFORM_WINDOWS / MMO_PLATFORM_LINUX 宏。
 *   2. 新增平台功能时，在这里声明接口，在 Platform_Win.cpp / Platform_Linux.cpp 中实现。
 *
 * 注意：
 *   - 大段平台逻辑（Dump、Crash Handler）→ 抽到 Platform_Win/Linux.cpp
 *   - 小段平台差异（2-5 行）→ 直接在调用处 inline #ifdef，不额外抽象
 *   - 全局 _WIN32_WINNT 在此处一次性定义
 */
#pragma once

// ── 平台检测 ──────────────────────────────────────────────
#if defined(_WIN32)
    #define MMO_PLATFORM_WINDOWS 1
#elif defined(__linux__)
    #define MMO_PLATFORM_LINUX 1
#else
    #error "Massive currently supports Windows and Linux only"
#endif

// ── Windows SDK 版本（Asio 等依赖此宏） ─────────────────
#if MMO_PLATFORM_WINDOWS
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601 // Windows 7+
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

#include <cstdint>
#include <filesystem>

namespace MMO::Platform
{

    // ── Process ──

    /**
     * @brief 获取可执行文件所在目录
     *
     * Windows: GetModuleFileNameW → dirname
     * Linux:   readlink /proc/self/exe → dirname
     *
     * fallback: current_path()
     */
    std::filesystem::path GetExeDir();

    // ── Console ──

    /**
     * @brief 检查 stdout 是否有效
     *
     * Windows DETACHED_PROCESS 场景子进程无控制台句柄，返回 false。
     * Linux 上始终返回 true。
     */
    bool IsStdoutAvailable();

    // ── Crash Dump ──

    /**
     * @brief 安装平台原生系统级 crash 处理器
     *
     * Windows: SetUnhandledExceptionFilter (SEH) + signal(SIGABRT)
     * Linux:   sigaction (SIGSEGV/SIGABRT/SIGILL/SIGFPE/SIGBUS/SIGTRAP)
     *
     * Crash 处理器内部会调用 WritePlatformDump + 写 .stacktrace 文本栈，
     * 然后 _exit(1)。
     */
    void InstallCrashHandlers();

    /**
     * @brief 写入平台原生可调试 dump 文件
     *
     * Windows: MiniDumpWriteDump → {dir}/{name}_crash_{pid}.dmp
     * Linux:   fork+abort → {dir}/{name}_crash_{pid}.core
     *
     * @param dir   输出目录
     * @param name  进程名
     * @param pid   进程 PID
     * @param ep    Windows 上的 EXCEPTION_POINTERS*, Linux 传 nullptr
     */
    void WritePlatformDump(const char *dir, const char *name, int pid, void *ep);

} // namespace MMO::Platform
