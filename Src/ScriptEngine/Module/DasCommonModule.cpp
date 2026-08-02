#include "DasCommonModule.h"
#include "Common/Log/Log.h"
#include "Proto/AutoGen/EMsgIDBind.gen.h"
#include "daScript/ast/ast.h"
#include "daScript/daScript.h"
#include "daScript/simulate/debug_info.h"
#include "daScript/simulate/simulate.h"

namespace MMO
{

    void LogInfo(const char *text, das::Context *ctx, das::LineInfoArg *at)
    {
        if (nullptr != at && nullptr != at->fileInfo)
        {
            Log::At(ELogLevel::Info,
                    at->fileInfo->name.c_str(),
                    at->line,
                    "{}",
                    nullptr != text ? text : "(null)");
        }
        else
        {
            Log::Info("{}", nullptr != text ? text : "(null)");
        }
    }

    void LogWarn(const char *text, das::Context *ctx, das::LineInfoArg *at)
    {
        if (nullptr != at && nullptr != at->fileInfo)
        {
            Log::At(ELogLevel::Warn,
                    at->fileInfo->name.c_str(),
                    at->line,
                    "{}",
                    nullptr != text ? text : "(null)");
        }
        else
        {
            Log::Warn("{}", nullptr != text ? text : "(null)");
        }
    }

    void LogError(const char *text, das::Context *ctx, das::LineInfoArg *at)
    {
        if (nullptr != at && nullptr != at->fileInfo)
        {
            Log::At(ELogLevel::Error,
                    at->fileInfo->name.c_str(),
                    at->line,
                    "{}",
                    nullptr != text ? text : "(null)");
        }
        else
        {
            Log::Error("{}", nullptr != text ? text : "(null)");
        }
    }

    DasCommonModule::DasCommonModule() : das::Module("Common")
    {
    }

    void DasCommonModule::Build()
    {
        das::ModuleLibrary lib(this);
        lib.addBuiltInModule();

        // Log —— ctx/at 是隐藏 interop 参数（脚本侧不可见），必须用约定名 "context"/"at"
        das::addExtern<DAS_BIND_FUN(LogInfo)>(*this,
                                              lib,
                                              "LogInfo",
                                              das::SideEffects::modifyExternal,
                                              "MMO::LogInfo")
            ->args({"text", "context", "at"});
        das::addExtern<DAS_BIND_FUN(LogWarn)>(*this,
                                              lib,
                                              "LogWarn",
                                              das::SideEffects::modifyExternal,
                                              "MMO::LogWarn")
            ->args({"text", "context", "at"});
        das::addExtern<DAS_BIND_FUN(LogError)>(*this,
                                               lib,
                                               "LogError",
                                               das::SideEffects::modifyExternal,
                                               "MMO::LogError")
            ->args({"text", "context", "at"});

        // EMsgID 枚举绑定——全局共享 ID 值空间，所有服务 require Common 即可引用
        RegisterEMsgIDEnumeration(*this);
    }

    das::ModuleAotType DasCommonModule::aotRequire(das::TextWriter &tw) const
    {
        tw << "#include \"ScriptEngine/Module/DasCommonModule.h\"\n";
        return das::ModuleAotType::cpp;
    }
} // namespace MMO