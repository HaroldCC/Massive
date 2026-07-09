/**
 * @file Stacktrace.cpp
 * @brief 栈回溯符号化 + InstallStackTrace 入口
 *
 * 职责:
 *   1. DumpStackTrace → absl::Symbolize 包装，供 MASSIVE_ASSERT 使用
 *   2. InstallStackTrace → 初始化 absl + 设置进程名 + 安装平台 crash handler
 *
 * SetCrashDumpDirectory 实现在 Dump.cpp 中（Stacktrace.h include 了 Dump.h）。
 */
#include "Common/Core/Stacktrace.h"
#include "Common/Core/Dump.h"
#include "Common/Core/Platform.h"

#include "absl/debugging/stacktrace.h"
#include "absl/debugging/symbolize.h"

#include <array>
#include <cstdio>
#include <cstring>

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

        std::array<char, 512> symBuf;
        for (int i = 0; i < depth; ++i)
        {
            if (absl::Symbolize(stack[i], symBuf.data(), static_cast<int>(symBuf.size())))
            {
                fprintf(out, "  #%-2d %s\n", i, symBuf.data());
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
        // 1. 初始化符号解析
        absl::InitializeSymbolizer(argv0);

        // 2. 缓存进程名
        const char *name = nullptr;
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
            name = base;
        }
        SetCrashDumpProcessName(name);

        // 3. 安装平台原生 crash dump handler
        Platform::InstallCrashHandlers();
    }

} // namespace MMO
