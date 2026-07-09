/**
 * @file Stacktrace.cpp
 * @brief 栈回溯实现 (Abseil debugging 封装)
 *
 * 双层 Crash 日志防御（确保宕服前栈回溯落盘）：
 *   1. FailureSignalHandler → raw write() 到 crash dump 文件 + stderr
 *   2. DumpStackTrace (非 signal-safe) → fprintf + spdlog 钩子
 *
 * crash dump 文件路径：{logDir}/{processName}_crash_{pid}.dmp
 * signal handler 中使用 signal-safe write()，不碰任何锁或 spdlog。
 */

#include "Common/Core/Stacktrace.h"

#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/stacktrace.h"
#include "absl/debugging/symbolize.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
    #include <io.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <windows.h>
    #include <process.h>
#else
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <fcntl.h>
#endif

namespace MMO
{

    namespace
    {

        /** @brief crash dump 文件的 fd（仅 signal handler 中写入，需保持 signal-safe） */
        std::atomic<int> g_crashDumpFd {-1};

        /** @brief 缓存进程名（仅 InstallStackTrace 中写入，其后只读） */
        char g_processName[128] = {0};

        /**
         * @brief signal-safe 的 itoa（只写整数到栈缓冲区）
         */
        [[maybe_unused]] const char *SignalSafeItoa(int val, char *buf, size_t bufSize)
        {
            // 反向填充
            char tmp[32];
            int  idx = 0;
            bool neg = val < 0;
            if (neg)
            {
                val = -val;
            }
            do
            {
                tmp[idx++] = static_cast<char>('0' + (val % 10));
                val /= 10;
            } while (val > 0 && idx < static_cast<int>(sizeof(tmp) - 1));

            int pos = 0;
            if (neg && pos < static_cast<int>(bufSize) - 1)
            {
                buf[pos++] = '-';
            }
            for (int i = idx - 1; i >= 0 && pos < static_cast<int>(bufSize) - 1; --i)
            {
                buf[pos++] = tmp[i];
            }
            buf[pos] = '\0';
            return buf;
        }

        /**
         * @brief signal-safe write (无论平台，都走 raw write)
         */
        void SafeWrite(int fd, const char *data, size_t len)
        {
            if (fd < 0)
            {
                return;
            }
#ifdef _WIN32
            ::_write(fd, data, static_cast<unsigned int>(len));
#else
            ::write(fd, data, len);
#endif
        }

        /**
         * @brief signal-safe 写字符串
         */
        void SafeWriteStr(int fd, const char *s)
        {
            if (fd < 0 || !s)
            {
                return;
            }
            SafeWrite(fd, s, std::strlen(s));
        }

        /**
         * @brief Abseil writerfn — 同时写入 crash dump 文件和 stderr
         */
        void CrashWriterFn(const char *data)
        {
            size_t len = std::strlen(data);

            // 始终写 stderr（终端可见）
#ifdef _WIN32
            ::_write(2, data, static_cast<unsigned int>(len));
#else
            ::write(STDERR_FILENO, data, len);
#endif

            // 额外写 crash dump 文件（守护进程场景）
            int fd = g_crashDumpFd.load(std::memory_order_relaxed);
            if (fd >= 0)
            {
                SafeWrite(fd, data, len);
            }
        }

        /**
         * @brief 打开 crash dump 文件（非 signal-safe，只在 InstallStackTrace 中调用）
         *
         * 尝试在 {logDir} 下创建 {name}_crash_{pid}.dmp，
         * 若 logDir 为空或创建失败，降级到 {exeDir}/crash_{name}_{pid}.dmp，
         * 再失败则只写 stderr（CrashWriterFn 已保底）。
         */
        void OpenCrashDumpFile(const char *logDir, const char *name)
        {
            if (!name || name[0] == '\0')
            {
                return;
            }

#ifdef _WIN32
            int pid = ::_getpid();
#else
            int pid = static_cast<int>(::getpid());
#endif

            // 尝试第一个路径：{logDir}/{name}_crash_{pid}.dmp
            std::string path;
            if (logDir && logDir[0] != '\0')
            {
                path = std::string(logDir) + "/" + name + "_crash_" + std::to_string(pid) + ".dmp";
            }

            if (path.empty())
            {
                // 降级：当前目录
                path = std::string(name) + "_crash_" + std::to_string(pid) + ".dmp";
            }

#ifdef _WIN32
            // Windows — _O_WRONLY | _O_CREAT | _O_TRUNC, 0644
            int fd = ::_open(path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC, _S_IREAD | _S_IWRITE);
#else
            int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif

            if (fd >= 0)
            {
                g_crashDumpFd.store(fd, std::memory_order_relaxed);
                // 写文件头
                const char header[] = "=== Crash Dump ===\n";
                SafeWrite(fd, header, sizeof(header) - 1);
            }
        }

    } // anonymous namespace

    void DumpStackTrace(FILE *out, int maxFrames, int skip)
    {
        if (!out)
        {
            return;
        }

        std::array<void *, 64> stack;
        maxFrames = maxFrames < 64 ? maxFrames : 64;
        int depth = absl::GetStackTrace(stack.data(), maxFrames, skip);

        fprintf(out, "=== Stack trace (%d frames) ===\n", depth);

        std::array<char, 512> buf;
        for (int i = 0; i < depth; ++i)
        {
            if (absl::Symbolize(stack[i], buf.data(), static_cast<int>(buf.size())))
            {
                fprintf(out, "  #%-2d %s\n", i, buf.data());
            }
            else
            {
                fprintf(out, "  #%-2d 0x%p\n", i, stack[i]);
            }
        }

        fflush(out);
    }

    void InstallStackTrace(const char *argv0)
    {
        // 初始化符号解析
        absl::InitializeSymbolizer(argv0);

        // 缓存进程名（从 argv0 提取 basename）
        if (argv0)
        {
            const char *base = std::strrchr(argv0, '/');
            if (!base)
            {
                base = std::strrchr(argv0, '\\');
            }
            if (base)
            {
                ++base;
            }
            else
            {
                base = argv0;
            }
            std::strncpy(g_processName, base, sizeof(g_processName) - 1);
            g_processName[sizeof(g_processName) - 1] = '\0';

            // 去掉 .exe 后缀（Windows）
            char *dot = std::strrchr(g_processName, '.');
            if (dot && (std::strcmp(dot, ".exe") == 0 || std::strcmp(dot, ".EXE") == 0))
            {
                *dot = '\0';
            }
        }

        // 尝试从 SPDLOG_LEVEL 环境变量推断 logDir（无依赖方式）
        // 真正的 logDir 由 Log::Init 知道，但 signal handler 不能依赖 spdlog
        // 降级：为空则创建到当前目录
        const char *logDirEnv = nullptr;
        (void)logDirEnv; // 暂不依赖环境变量，由外部显式调用 SetCrashDumpDir

        // 安装 failure signal handler: crash 时写 stderr + crash dump 文件
        absl::FailureSignalHandlerOptions opts;
        opts.writerfn = CrashWriterFn;
        absl::InstallFailureSignalHandler(opts);
    }

    void SetCrashDumpDirectory(const char *logDir)
    {
        if (!g_processName[0])
        {
            // InstallStackTrace 还没调用就用默认名称
            std::strncpy(g_processName, "unknown", sizeof(g_processName) - 1);
        }
        OpenCrashDumpFile(logDir, g_processName);
    }

} // namespace MMO
