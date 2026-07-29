#pragma once

#include "Common/Timer/TimingWheel.h"
#include "ScriptEngine/IDasHost.h"
#include "daScript/ast/ast.h"
#include "daScript/daScriptModule.h"
#include "daScript/simulate/simulate.h"
#include <memory>

namespace MMO
{
    class DasCommonModule : public das::Module
    {
    public:
        DasCommonModule();

        void SetHost(IDasLangtHost *host, TimingWheel *timingWheel);

        void BindFunctions();

        das::Context *GetContext() const;

        void SetContext(std::shared_ptr<das::Context> ctx);

    private:
        IDasLangtHost                *_host        = nullptr;
        TimingWheel                  *_timingWheel = nullptr;
    };
} // namespace MMO