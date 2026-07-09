/**
 * @file Platform_Linux.cpp
 * @brief Linux 平台实现
 *
 * 实现 Platform.h 中声明的平台抽象函数。
 */
#include "Common/Core/Platform.h"

#if !MMO_PLATFORM_LINUX
    #error "This file is Linux-only"
#endif

#include "Common/Core/Dump.h" // MMO::CrashHandlerEntry

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <limits.h>

#include <cstdlib>
#include <cstring>

namespace MMO::Platform
{

    // ── Process ──

    std::filesystem::path GetExeDir()
    {
        std::error_code ec;

        char    buf[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0)
        {
            buf[len] = '\0';
            return std::filesystem::path(std::string(buf)).parent_path();
        }

        return std::filesystem::current_path(ec);
    }

    // ── Console ──

    bool IsStdoutAvailable()
    {
        // Linux: stdout 始终有效，即使重定向到 /dev/null
        return true;
    }

    // ── Crash Dump ──

    namespace
    {

        /**
         * @brief 忙等待子进程结束并重命名 core 文件
         *
         * 父进程在 signal handler 中，需要等内核写完 core 后改名。
         * 不能用 blocking waitpid，繁忙轮询 WNOHANG。
         *
         * rename: core.{childPid} → {dir}/{name}_crash_{pid}.core
         */
        static void WaitChildAndRename(pid_t childPid, const char *dir, const char *name, int pid)
        {
            // 忙等待：最多等 10 秒
            const int maxTries = 50; // 50 × 200ms = 10s
            for (int i = 0; i < maxTries; ++i)
            {
                int   status = 0;
                pid_t ret    = ::waitpid(childPid, &status, WNOHANG);

                if (ret == childPid)
                {
                    break; // 子进程结束，core 应已写完毕
                }

                if (ret < 0 && errno != EAGAIN && errno != EINTR)
                {
                    break; // 不可恢复
                }

                // 200ms 延时（nanosleep 是 signal-safe 的）
                struct timespec ts;
                ts.tv_sec  = 0;
                ts.tv_nsec = 200000000;
                ::nanosleep(&ts, nullptr);
            }

            // 保底回收
            int finalStatus = 0;
            ::waitpid(childPid, &finalStatus, WNOHANG);
            (void)finalStatus;

            // ── 重命名 core.{childPid} → {dir}/{name}_crash_{pid}.core ──
            // 构建目标路径
            char         target[512];
            size_t       dlen     = std::strlen(dir);
            size_t       nlen     = std::strlen(name);
            const size_t overhead = 64;
            if (dlen + 1 + nlen + overhead > sizeof(target))
            {
                return;
            }

            char *p = target;
            std::memcpy(p, dir, dlen);
            p += dlen;
            if (dlen > 0 && dir[dlen - 1] != '/' && dir[dlen - 1] != '\\')
            {
                *p++ = '/';
            }
            std::memcpy(p, name, nlen);
            p += nlen;
            int n = snprintf(p, target + sizeof(target) - p, "_crash_%d.core", pid);
            if (n < 0)
            {
                return;
            }
            (void)n;

            // 先尝试 core.{childPid}
            char coreName[64];
            n = snprintf(coreName, sizeof(coreName), "core.%d", static_cast<int>(childPid));
            if (n > 0 && static_cast<size_t>(n) < sizeof(coreName))
            {
                if (::rename(coreName, target) == 0)
                {
                    return;
                }
            }

            // 回退：尝试裸 core
            ::rename("core", target);
        }

    } // anonymous namespace

    /**
     * @brief POSIX 崩溃信号处理器（文件内可见，非匿名）
     *
     * sigaction 需要 C linkage 兼容的 handler。
     */
    static void PosixSignalHandler(int)
    {
        MMO::CrashHandlerEntry(nullptr); // [[noreturn]]
    }

    void InstallCrashHandlers()
    {
        // sigaction 覆盖所有致命信号
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = PosixSignalHandler;
        ::sigfillset(&sa.sa_mask); // handler 期间屏蔽所有信号
        sa.sa_flags = SA_NODEFER;  // 允许嵌套（fork 后 abort 触发 SIGABRT）

        ::sigaction(SIGSEGV, &sa, nullptr);
        ::sigaction(SIGABRT, &sa, nullptr);
        ::sigaction(SIGILL, &sa, nullptr);
        ::sigaction(SIGFPE, &sa, nullptr);
        ::sigaction(SIGBUS, &sa, nullptr);
        ::sigaction(SIGTRAP, &sa, nullptr);

        // 启动时允许 core dump
        struct rlimit rl;
        if (::getrlimit(RLIMIT_CORE, &rl) == 0)
        {
            rl.rlim_cur = RLIM_INFINITY;
            ::setrlimit(RLIMIT_CORE, &rl);
        }
    }

    void WritePlatformDump(const char *dir, const char *name, int pid, void *ep)
    {
        (void)ep;

        // fork → child abort → 内核写 core → 父进程 waitpid + rename
        pid_t childPid = ::fork();
        if (childPid == 0)
        {
            // 子进程：切到 dir，允许 core，重置信号，abort
            if (dir && dir[0] != '\0')
            {
                ::chdir(dir);
            }

            struct rlimit rl;
            rl.rlim_cur = RLIM_INFINITY;
            rl.rlim_max = RLIM_INFINITY;
            ::setrlimit(RLIMIT_CORE, &rl);

            // 重置信号到默认
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = SIG_DFL;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGABRT, &sa, nullptr);
            sigaction(SIGSEGV, &sa, nullptr);
            sigaction(SIGILL, &sa, nullptr);
            sigaction(SIGFPE, &sa, nullptr);
            sigaction(SIGBUS, &sa, nullptr);
            sigaction(SIGTRAP, &sa, nullptr);

            ::abort();
            ::_exit(1); // unreachable
        }

        // 父进程：等待子进程结束，重命名 core
        if (childPid > 0)
        {
            WaitChildAndRename(childPid, dir, name, pid);
        }
    }

} // namespace MMO::Platform
