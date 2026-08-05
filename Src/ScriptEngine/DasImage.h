#pragma once

#include "daScript/ast/ast.h"
#include "daScript/simulate/debug_info.h"
#include "daScript/simulate/simulate.h"
#include <memory>

namespace MMO
{
    struct DasLangImage
    {
        std::unique_ptr<das::ModuleGroup> moduleGroup;
        das::FileAccessPtr                fileAccess;
        das::ProgramPtr                   program;
        std::shared_ptr<das::Context>     ctx;

        // 缓存的入口函数——裸指针，生命周期随 ctx（shared_ptr）存活：
        // 只要 ctx 未释放，SimFunction 指向 Context 函数表的指针就有效。
        // DoSwap 整图替换后必须 RebindFunctions 重抓（旧指针随旧 ctx 释放而失效）。
        // 与官方 daslang-live 的 CompileResult（裸 fnInit/fnUpdate）同款模式。
        das::SimFunction *funcInit        = nullptr;
        das::SimFunction *funcUpdate      = nullptr;
        das::SimFunction *funcDispatchMsg = nullptr;

        std::string errors;

        bool IsValid() const
        {
            return ctx != nullptr;
        }
    };
} // namespace MMO