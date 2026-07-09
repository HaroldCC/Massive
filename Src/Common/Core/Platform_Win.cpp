/**
 * @file Platform_Win.cpp
 * @brief Windows 平台实现
 *
 * 实现 Platform.h 中声明的平台抽象函数。
 */
#include "Common/Core/Platform.h"

#if !MMO_PLATFORM_WINDOWS
    #error "This file is Windows-only"
#endif

#include "Common/Core/Dump.h" // MMO::CrashHandlerEntry

#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <sys/stat.h>
#include <dbghelp.h>
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <csignal>

namespace MMO::Platform
{

    // ── Process ──

    std::filesystem::path GetExeDir()
    {
        std::error_code ec;

        wchar_t buf[MAX_PATH];
        DWORD   len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH)
        {
            return std::filesystem::path(buf).parent_path();
        }

        return std::filesystem::current_path(ec);
    }

    // ── Console ──

    bool IsStdoutAvailable()
    {
        HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
        return hOut != nullptr && hOut != INVALID_HANDLE_VALUE;
    }

    // ── Crash Dump ──

    namespace
    {

        /**
         * @brief MiniDump 写入（EXCEPTION_POINTERS 由调用方提供）
         */
        static void WriteMiniDump(const char *dmpPath, void *ep)
        {
            HANDLE hFile = CreateFileA(dmpPath,
                                       GENERIC_WRITE,
                                       0,
                                       nullptr,
                                       CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr);
            if (hFile == INVALID_HANDLE_VALUE)
            {
                return;
            }

            MINIDUMP_EXCEPTION_INFORMATION mei;
            memset(&mei, 0, sizeof(mei));
            mei.ThreadId          = GetCurrentThreadId();
            mei.ExceptionPointers = static_cast<EXCEPTION_POINTERS *>(ep);
            mei.ClientPointers    = FALSE;

            MiniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                hFile,
                static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs),
                &mei,
                nullptr,
                nullptr);

            CloseHandle(hFile);
        }

        /**
         * @brief Windows SEH 未处理异常过滤器
         *
         * 有真实的 EXCEPTION_POINTERS，传入 CrashHandlerEntry 用于 MiniDump。
         */
        static LONG WINAPI SehHandler(EXCEPTION_POINTERS *ep)
        {
            // 交由 Dump.cpp 的统一入口处理（文本栈 + MiniDump + _exit）
            MMO::CrashHandlerEntry(ep);       // [[noreturn]]
            return EXCEPTION_EXECUTE_HANDLER; // unreachable
        }

        /**
         * @brief Windows SIGABRT 处理器
         *
         * abort() / assert() 触发 SIGABRT，不经过 SEH，无 EXCEPTION_POINTERS。
         */
        static void SigAbortHandler(int)
        {
            MMO::CrashHandlerEntry(nullptr); // [[noreturn]]
        }

    } // anonymous namespace

    void InstallCrashHandlers()
    {
        // 1. SEH 顶层兜底：所有没有 __try/__except 的硬件异常
        SetUnhandledExceptionFilter(SehHandler);

        // 2. SIGABRT: abort() / assert() 不经过 SEH
        signal(SIGABRT, SigAbortHandler);

        // 注：不覆盖 SIGSEGV/SIGILL/SIGFPE —— Windows 下由 SEH 传递。
        //     SIGBREAK (Ctrl+Break) 由 GracefulShutdown 处理。
    }

    void WritePlatformDump(const char *dir, const char *name, int pid, void *ep)
    {
        // 构建路径: {dir}/{name}_crash_{pid}.dmp
        // 用栈缓冲区手动拼接（signal-safe 场景避免 heap）
        char   dmpPath[512];
        size_t dlen = std::strlen(dir);
        size_t nlen = std::strlen(name);

        // 安全检查：总长度
        // dir + '/' + name + '_crash_' + pid_str + '.dmp' + '\0'
        const size_t overhead = 64; // 足够容纳后缀
        if (dlen + 1 + nlen + overhead > sizeof(dmpPath))
        {
            return;
        }

        char *p = dmpPath;
        std::memcpy(p, dir, dlen);
        p += dlen;
        if (dlen > 0 && dir[dlen - 1] != '/' && dir[dlen - 1] != '\\')
        {
            *p++ = '/';
        }
        std::memcpy(p, name, nlen);
        p += nlen;
        int n = snprintf(p, dmpPath + sizeof(dmpPath) - p, "_crash_%d.dmp", pid);
        if (n < 0)
        {
            return;
        }
        (void)n;

        WriteMiniDump(dmpPath, ep);
    }

} // namespace MMO::Platform
