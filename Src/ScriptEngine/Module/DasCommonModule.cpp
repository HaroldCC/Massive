#include "DasCommonModule.h"
#include "Common/Log/Log.h"
#include "daScript/ast/ast.h"
#include "daScript/daScript.h"
#include "daScript/simulate/debug_info.h"
#include "daScript/simulate/simulate.h"

namespace MMO
{

    static void LogInfo(const char *text, das::Context *ctx, das::LineInfoArg *at)
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

    static void LogWarn(const char *text, das::Context *ctx, das::LineInfoArg *at)
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

    static void LogError(const char *text, das::Context *ctx, das::LineInfoArg *at)
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

    void DasCommonModule::SetHost(IDasLangtHost *host, TimingWheel *timingWheel)
    {
        _host        = host;
        _timingWheel = timingWheel;
    }

    void DasCommonModule::BindFunctions()
    {
        das::ModuleLibrary lib(this);
        lib.addBuiltInModule();

        // Log
        das::addExtern<DAS_BIND_FUN(LogInfo)>(*this, lib, "LogInfo", das::SideEffects::modifyExternal)
            ->args({"text", "ctx", "at"});
        das::addExtern<DAS_BIND_FUN(LogWarn)>(*this, lib, "LogWarn", das::SideEffects::modifyExternal)
            ->args({"text", "ctx", "at"});
        das::addExtern<DAS_BIND_FUN(LogError)>(*this, lib, "LogError", das::SideEffects::modifyExternal)
            ->args({"text", "ctx", "at"});
    }

    das::Context *DasCommonModule::GetContext() const
    {
        return _host->GetScriptContext();
    }

    REGISTER_DYN_MODULE(DasCommonModule, DasCommonModule)
    REGISTER_MODULE(DasCommonModule)
} // namespace MMO