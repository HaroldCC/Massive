#include "DasEngine.h"
#include "DasEngineConfig.h"

#include "daScript/ast/ast.h"
#include "daScript/misc/smart_ptr.h"
#include "daScript/misc/string_writer.h"
#include "daScript/misc/sysos.h"
#include "daScript/daScriptModule.h"
#include "daScript/simulate/debug_info.h"
#include "daScript/simulate/fs_file_info.h"
#include "daScript/simulate/runtime_string.h"
#include "daScript/simulate/simulate.h"
#include <memory>

// DECLARE_ALL_DEFAULT_MODULES;

namespace MMO
{
    DasLangEngine &DasLangEngine::GetIns()
    {
        static DasLangEngine ins;
        return ins;
    }

    bool DasLangEngine::Initialize(const DasLangEngineConfig &cfg)
    {
        if (_initialized)
        {
            return true;
        }

        _dasRoot = cfg.dasLangRoot;
        das::setDasRoot(_dasRoot);

        // PULL_ALL_DEFAULT_MODULES;
        NEED_ALL_DEFAULT_MODULES;
        das::Module::Initialize();

        _initialized = true;
        return true;
    }

    void DasLangEngine::Shutdown()
    {
        if (!_initialized)
        {
            return;
        }

        das::Module::Shutdown();
        _initialized = false;
    }

    das::ProgramPtr DasLangEngine::CompileScript(const std::string &mainFile, das::ModuleGroup &libGroup)
    {
        _lastCompileErrors.clear();

        auto fAccess = das::make_smart<das::FsFileAccess>();
        fAccess->introduceDaslib();

        das::TextWriter logs;
        auto            program = das::compileDaScript(mainFile, fAccess, logs, libGroup);
        if (!program)
        {
            _lastCompileErrors = "program is null";
            return nullptr;
        }

        if (program->failed())
        {
            for (auto &err : program->errors)
            {
                logs << das::reportError(err.at, err.what, err.extra, err.fixme, err.cerr);
            }

            _lastCompileErrors = logs.str();
            return nullptr;
        }

        return program;
    }

    std::shared_ptr<das::Context> DasLangEngine::CreateContext(das::ProgramPtr program)
    {
        if (!program)
        {
            return nullptr;
        }

        return std::make_shared<das::Context>(program->getContextStackSize());
    }

    const std::string &DasLangEngine::GetLastCompileErrors() const
    {
        return _lastCompileErrors;
    }
} // namespace MMO