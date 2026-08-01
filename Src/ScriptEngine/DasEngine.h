#pragma once

#include "Common/Core/Types.h"
#include "DasEngineConfig.h"
#include "DasFileWatcher.h"
#include "DasImage.h"
#include "ScriptEngine/IDasHost.h"
#include "ScriptEngine/ScriptDispatchRegistry.h"
#include "daScript/ast/ast.h"
#include "daScript/simulate/simulate.h"
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace MMO
{
    class IDasLangModuleProvider;
    class DasCommonModule;

    class DasLangEngine
    {
    public:
        static DasLangEngine &GetIns();

        bool Initialize(const DasLangEngineConfig &cfg,
                        IDasLangHost              *host,
                        IDasLangModuleProvider    *moduleProvider);
        void Shutdown();

        bool Load(const std::string &entryFile);

        void Tick(float dt);

        /**
         * @brief 重载补丁
         * @param dasbinPath 补丁脚本二进制文件
         */
        void RequestReload(const std::string &dasbinPath);

        /**
         * @brief 从脚本源码重载
         */
        void RequestReloadFromSource();

        das::Context *GetScriptContext() const
        {
            return _scriptImage.ctx.get();
        }

        das::SimFunction *GetDispatchFunc() const
        {
            return _scriptImage.funcDispatchMsg;
        }

        const std::string &GetLastErrors() const
        {
            return _scriptImage.errors;
        }

        static das::CodeOfPolicies MakePolicies(EScriptMode mode, bool forAotGen);

        ScriptDispatchRegistry &DispatchRegistry()
        {
            return _scriptDispatcher;
        }

    private:
        DasLangEngine()                                 = default;
        ~DasLangEngine()                                = default;
        DasLangEngine(const DasLangEngine &)            = delete;
        DasLangEngine &operator=(const DasLangEngine &) = delete;

        DasLangImage Compile(const std::string &enteryFile, const std::string &dasbinPath);

        bool                     SimulateImage(DasLangImage &img);
        void                     RebindFunctions(DasLangImage &img);
        void                     DoSwap(DasLangImage &&img);
        void                     PollReload();
        std::vector<std::string> CollectDependencyFiles() const;

        void BuildModuleGroup(DasLangImage &img);

    private:
        DasLangEngineConfig              _cfg;
        IDasLangHost                    *_scriptHost     = nullptr;
        IDasLangModuleProvider          *_moduleProvider = nullptr;
        std::unique_ptr<DasCommonModule> _commonModule;
        DasLangImage                     _scriptImage;
        std::string                      _entryFile;

        std::atomic<bool> _reloadPending {false};
        std::string       _pendingDasbin;
        std::mutex        _reloadMutex;

        std::unique_ptr<DasFileWatcher> _scriptFileWatcher;
        uint64                          _lastGCHeapSize {0};
        bool                            _initialized = false;

        ScriptDispatchRegistry _scriptDispatcher;
    };
} // namespace MMO