#pragma once

#include "DasEngineConfig.h"
#include "daScript/ast/ast.h"
#include "daScript/simulate/simulate.h"
#include <memory>

namespace MMO
{
    class DasLangEngine
    {
    public:
        static DasLangEngine &GetIns();

        bool Initialize(const DasLangEngineConfig &cfg);

        void Shutdown();

        das::ProgramPtr CompileScript(const std::string &mainFile, das::ModuleGroup &libGroup);

        std::shared_ptr<das::Context> CreateContext(das::ProgramPtr program);

        const std::string &GetLastCompileErrors() const;

    private:
        DasLangEngine() = default;

        std::string _dasRoot;
        std::string _lastCompileErrors;
        bool        _initialized = false;
    };
} // namespace MMO