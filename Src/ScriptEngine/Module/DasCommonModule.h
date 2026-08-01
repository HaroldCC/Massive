#pragma once

#include "Common/Timer/TimingWheel.h"
#include "ScriptEngine/IDasHost.h"
#include "daScript/ast/ast.h"
#include "daScript/daScriptModule.h"
#include "daScript/misc/string_writer.h"
#include "daScript/simulate/simulate.h"
#include <memory>

namespace MMO
{

    void LogInfo(const char *text, das::Context *ctx, das::LineInfoArg *at);
    void LogWarn(const char *text, das::Context *ctx, das::LineInfoArg *at);
    void LogError(const char *text, das::Context *ctx, das::LineInfoArg *at);

    class DasCommonModule : public das::Module
    {
    public:
        DasCommonModule();

        void Build();

        das::ModuleAotType aotRequire(das::TextWriter &tw) const override;

    private:
        IDasLangHost *_host        = nullptr;
        TimingWheel  *_timingWheel = nullptr;
    };
} // namespace MMO