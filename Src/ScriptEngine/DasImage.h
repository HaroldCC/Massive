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

        // 缓存的入口函数
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