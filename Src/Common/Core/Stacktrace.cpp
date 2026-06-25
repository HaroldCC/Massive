/**
 * @file Stacktrace.cpp
 * @brief 栈回溯实现 (Abseil debugging 封装)
 */

#include "Common/Core/Stacktrace.h"

#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/stacktrace.h"
#include "absl/debugging/symbolize.h"

#include <array>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace MMO
{

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
        // 初始化符号解析 -- 必须在任何 Symbolize() 调用之前
        absl::InitializeSymbolizer(argv0);

        // 安装 failure signal handler: crash 时打印 signal-safe 栈回溯到 stderr
        absl::FailureSignalHandlerOptions opts;
        opts.writerfn = [](const char *data) {
            // writerfn 签名为 void(const char*) -- Abseil 使用 strlen 获取长度
            // 直接写 STDERR_FILENO, 绕过所有 I/O 缓冲
            size_t len = std::strlen(data);
#ifdef _WIN32
            ::_write(2, data, static_cast<unsigned int>(len));
#else
            ::write(STDERR_FILENO, data, len);
#endif
        };
        absl::InstallFailureSignalHandler(opts);
    }

} // namespace MMO
