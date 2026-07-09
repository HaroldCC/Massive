/**
 * @file Dump.cpp
 * @brief Crash Dump 编排实现
 *
 * 纯 cross-platform 逻辑，平台差异在 Platform_*.cpp 中。
 * 负责：
 *   1. 文本栈写入（absl::GetStackTrace, raw PC 地址, signal-safe）
 *   2. 编排 CrashHandlerEntry（文本栈 → Platform::WritePlatformDump → _exit）
 *   3. SetCrashDumpDirectory / SetCrashDumpProcessName 全局状态管理
 */

// MSVC: _open/_close are used in signal-safe paths
#ifdef _MSC_VER
    #define _CRT_SECURE_NO_WARNINGS
#endif

#include "Common/Core/Dump.h"
#include "Common/Core/Platform.h"

#include "absl/debugging/stacktrace.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>

#ifdef MMO_PLATFORM_WINDOWS
    #include <io.h>
    #include <process.h>
    #include <sys/stat.h>
#else
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace MMO
{

    namespace
    {

        // ── 全局状态（初始化后只读，signal-safe） ──

        /** @brief crash dump 目录缓冲 */
        char g_dumpDirBuf[260] = {0};

        /** @brief 目录指针（原子读） */
        std::atomic<const char *> g_dumpDir {nullptr};

        /** @brief 进程名缓冲 */
        char g_processNameBuf[128] = {0};

        // ── signal-safe 辅助 ──

        static void SafeStrCpy(char *dst, size_t dstSize, const char *src)
        {
            if (!dst || dstSize == 0 || !src)
            {
                return;
            }
            size_t i = 0;
            while (i < dstSize - 1 && src[i] != '\0')
            {
                dst[i] = src[i];
                ++i;
            }
            dst[i] = '\0';
        }

#ifdef MMO_PLATFORM_WINDOWS
        static void SafeWrite(int fd, const char *data, size_t len)
        {
            ::_write(fd, data, static_cast<unsigned int>(len));
        }

        static void SafeClose(int fd)
        {
            ::_close(fd);
        }

        static int SafeOpen(const char *path, int flags, int mode)
        {
            (void)mode;
            return ::_open(path, flags, _S_IREAD | _S_IWRITE);
        }
#else
        static void SafeWrite(int fd, const char *data, size_t len)
        {
            ::write(fd, data, len);
        }

        static void SafeClose(int fd)
        {
            ::close(fd);
        }

        static int SafeOpen(const char *path, int flags, int mode)
        {
            return ::open(path, flags, mode);
        }
#endif

        /**
         * @brief 构建文件路径: {dir}/{name}_crash_{pid}{ext}
         */
        static int
        BuildPath(char *buf, size_t bufSize, const char *dir, const char *name, int pid, const char *ext)
        {
            if (!buf || bufSize == 0)
            {
                return -1;
            }

            size_t pos = 0;

            // dir
            if (dir)
            {
                while (pos < bufSize - 1 && dir[pos] != '\0')
                {
                    buf[pos] = dir[pos];
                    ++pos;
                }
            }
            buf[pos] = '\0';

            // trailing '/'
            if (pos > 0 && buf[pos - 1] != '/' && buf[pos - 1] != '\\')
            {
                if (pos < bufSize - 1)
                {
                    buf[pos] = '/';
                    ++pos;
                    buf[pos] = '\0';
                }
            }

            // name_crash_pid.ext
            int remaining = static_cast<int>(bufSize) - static_cast<int>(pos) - 1;
            if (remaining <= 0)
            {
                return -1;
            }

            int n = snprintf(buf + pos, static_cast<size_t>(remaining), "%s_crash_%d%s", name, pid, ext);
            if (n < 0 || static_cast<size_t>(n) >= static_cast<size_t>(remaining))
            {
                return -1;
            }
            return 0;
        }

        // ── 文本栈（raw PC 地址，signal-safe） ──

        /**
         * @brief 写 .stacktrace 文件
         * @return 0 成功，-1 失败
         */
        static int WriteTextStack(const char *dir, const char *name, int pid)
        {
            char path[512];
            if (BuildPath(path, sizeof(path), dir, name, pid, ".stacktrace") != 0)
            {
                return -1;
            }

#ifdef MMO_PLATFORM_WINDOWS
            int fd = SafeOpen(path, _O_WRONLY | _O_CREAT | _O_TRUNC, 0);
#else
            int fd = SafeOpen(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
            if (fd < 0)
            {
                return -1;
            }

            const char header[] = "=== Crash Stack --- raw PC addresses ===\n";
            SafeWrite(fd, header, sizeof(header) - 1);

            void *stack[64];
            int   depth = absl::GetStackTrace(stack, 64, 1);

            char line[128];
            for (int i = 0; i < depth; ++i)
            {
                int n = snprintf(line, sizeof(line), "  #%-2d 0x%p\n", i, stack[i]);
                if (n > 0)
                {
                    auto uLen = static_cast<size_t>(n);
                    if (uLen >= sizeof(line))
                    {
                        uLen = sizeof(line) - 1;
                    }
                    SafeWrite(fd, line, uLen);
                }
            }

            SafeClose(fd);
            return 0;
        }

    } // anonymous namespace

    // ── 公开接口 ──

    void SetCrashDumpDirectory(const char *logDir)
    {
        if (logDir)
        {
            SafeStrCpy(g_dumpDirBuf, sizeof(g_dumpDirBuf), logDir);
            g_dumpDir.store(g_dumpDirBuf, std::memory_order_release);
        }
    }

    void SetCrashDumpProcessName(const char *name)
    {
        if (name)
        {
            SafeStrCpy(g_processNameBuf, sizeof(g_processNameBuf), name);
        }
    }

    [[noreturn]] void CrashHandlerEntry(void *ep)
    {
        const char *dir  = g_dumpDir.load(std::memory_order_acquire);
        const char *name = g_processNameBuf;

        if (!dir || dir[0] == '\0')
        {
            dir = ".";
        }
        if (!name || name[0] == '\0')
        {
            name = "unknown";
        }

#ifdef MMO_PLATFORM_WINDOWS
        int pid = ::_getpid();
#else
        int pid = static_cast<int>(::getpid());
#endif

        // 1. 文本栈
        WriteTextStack(dir, name, pid);

        // 2. 平台原生 dump
        Platform::WritePlatformDump(dir, name, pid, ep);

        // 3. 终止
        ::_exit(1);
    }

} // namespace MMO
