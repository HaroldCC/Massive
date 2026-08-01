#include "DasEngine.h"
#include "Common/Core/Types.h"
#include "DasEngineConfig.h"

#include "DasFileWatcher.h"
#include "DasImage.h"
#include "DasSerializer.h"
#include "IDasHost.h"
#include "IDasModuleProvider.h"
#include "Module/DasCommonModule.h"
#include "daScript/ast/ast.h"
#include "daScript/misc/smart_ptr.h"
#include "daScript/misc/string_writer.h"
#include "daScript/misc/sysos.h"
#include "daScript/daScriptModule.h"
#include "daScript/simulate/debug_info.h"
#include "daScript/simulate/fs_file_info.h"
#include "daScript/simulate/runtime_string.h"
#include "daScript/simulate/simulate.h"
#include "Common/Log/Log.h"
#include "vecmath/dag_vecMathDecl.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

DECLARE_ALL_DEFAULT_MODULES;

namespace MMO
{
    DasLangEngine &DasLangEngine::GetIns()
    {
        static DasLangEngine ins;
        return ins;
    }

    bool DasLangEngine::Initialize(const DasLangEngineConfig &cfg,
                                   IDasLangHost              *host,
                                   IDasLangModuleProvider    *moduleProvider)
    {
        if (_initialized)
        {
            return true;
        }

        _cfg            = cfg;
        _scriptHost     = host;
        _moduleProvider = moduleProvider;

        das::setDasRoot(cfg.dasLangRoot);

        PULL_ALL_DEFAULT_MODULES;

        _commonModule = std::make_unique<DasCommonModule>();
        _commonModule->Build();

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

        if (nullptr != _scriptFileWatcher)
        {
            _scriptFileWatcher->Stop();
        }

        if (nullptr != _moduleProvider)
        {
            _moduleProvider->DrainTimers();
        }

        _scriptImage = {}; // 释放旧的
        _commonModule.reset();

        das::Module::Shutdown();
        _initialized = false;
    }

    bool DasLangEngine::Load(const std::string &entryFile)
    {
        _entryFile = entryFile;

        // Release 优先尝试 .dasbin Develop直接源码编译
        std::string dasbinPath = "";
        if (_cfg.mode == EScriptMode::Release && !_cfg.dasbinDir.empty())
        {
            dasbinPath = std::format("{}/{}.dasbin", _cfg.dasbinDir, entryFile);
        }

        DasLangImage img = Compile(_entryFile, dasbinPath);
        if (!img.program)
        {
            Log::Error("Compile entryFile:{} error:{}", entryFile, img.errors);
            return false;
        }

        if (!SimulateImage(img))
        {
            Log::Error("Simulate failed:{}", img.errors);
            return false;
        }

        // save dasbin
        if (_cfg.mode == EScriptMode::Release && !_cfg.dasbinDir.empty() && dasbinPath.empty())
        {
            std::vector<std::pair<std::string, int64>> depFiles;
            for (auto *file : img.ctx->getAllFiles())
            {
                if (nullptr != file && !file->name.empty())
                {
                    depFiles.emplace_back(file->name, img.fileAccess->getFileMtime(file->name));
                }
            }

            std::string outPath = std::format("{}/{}.dasbin", _cfg.dasbinDir, entryFile);
            DasLangSerializer::Save(outPath, img.program, *img.moduleGroup, depFiles, _cfg.dasbinKeyHex);
        }

        RebindFunctions(img);

        if (nullptr != img.funcInit)
        {
            img.ctx->evalWithCatch(img.funcInit);
            if (auto ex = img.ctx->getException())
            {
                Log::Error("Called Init exception:{}", ex);
            }
        }

        _scriptImage    = std::move(img);
        _lastGCHeapSize = 0;

        // Develop 模式启用文件监视
        if (_cfg.mode == EScriptMode::Develop && _cfg.enableWatcher)
        {
            if (nullptr == _scriptFileWatcher)
            {
                _scriptFileWatcher = std::make_unique<DasFileWatcher>();
            }

            _scriptFileWatcher->Start(CollectDependencyFiles(), _cfg.watchPollMs, [this]() {
                RequestReloadFromSource();
            });
        }

        return true;
    }

    void DasLangEngine::Tick(float dt)
    {
        PollReload();

        if (!_scriptImage.IsValid())
        {
            return;
        }

        _moduleProvider->OnPrevTick(dt);
        _scriptImage.ctx->restart();

        if (nullptr != _scriptImage.funcUpdate)
        {
            vec4f args[] = {das::cast<float>::from(dt)};
            _scriptImage.ctx->evalWithCatch(_scriptImage.funcUpdate, args);
            if (auto ex = _scriptImage.ctx->getException())
            {
                Log::Error("Called Update exception:{}", ex);
            }
        }

        uint64 heapNow = _scriptImage.ctx->heap->getTotalBytesAllocated();
        if (heapNow - _lastGCHeapSize > (4 * 1024 * 1024))
        {
            _scriptImage.ctx->collectHeap(nullptr, true, true);
            _lastGCHeapSize = _scriptImage.ctx->heap->getTotalBytesAllocated();
        }
    }

    /**
     * @brief 重载补丁
     * @param dasbinPath 补丁脚本二进制文件
     */
    void DasLangEngine::RequestReload(const std::string &dasbinPath)
    {
        {
            std::lock_guard lg(_reloadMutex);
            _pendingDasbin = dasbinPath;
            _reloadPending.store(true, std::memory_order_release);
        }
    }

    /**
     * @brief 从脚本源码重载
     */
    void DasLangEngine::RequestReloadFromSource()
    {
        {
            std::lock_guard lg(_reloadMutex);
            _pendingDasbin.clear();
            _reloadPending.store(true, std::memory_order_release);
        }
    }

    DasLangImage DasLangEngine::Compile(const std::string &enteryFile, const std::string &dasbinPath)
    {
        DasLangImage img;
        BuildModuleGroup(img);

        img.fileAccess = das::make_smart<das::FsFileAccess>();
        static_cast<das::FsFileAccess *>(img.fileAccess.get())->introduceDaslib();

        // 先试.dasbin
        if (!dasbinPath.empty())
        {
            std::string err;
            auto        program = DasLangSerializer::Load(dasbinPath,
                                                          *img.moduleGroup,
                                                          img.fileAccess.get(),
                                                          _cfg.dasbinKeyHex,
                                                          err);
            if (program)
            {
                img.program = program;
                return img;
            }

            Log::Info("dasbin miss:({}), fall back to full compile", err);

            // Load失败可能把library弄脏，重建moduleGroup
            BuildModuleGroup(img);
        }

        // 全量编译
        das::TextWriter logs;
        img.program = das::compileDaScript(enteryFile,
                                           img.fileAccess,
                                           logs,
                                           *img.moduleGroup,
                                           MakePolicies(_cfg.mode, false));
        if (!img.program || img.program->failed())
        {
            img.errors = logs.str();
            Log::Error("Compile [{}] failed:{}", enteryFile, img.errors);
            img.program = nullptr;
        }

        return img;
    }

    bool DasLangEngine::SimulateImage(DasLangImage &img)
    {
        img.ctx = std::make_shared<das::Context>(img.program->getContextStackSize());
        _moduleProvider->OnContextSwapped(img.ctx);

        das::TextWriter logs;
        if (!img.program->simulate(*img.ctx, logs))
        {
            img.errors = logs.str();
            img.ctx    = nullptr;
            return false;
        }

        return true;
    }

    void DasLangEngine::RebindFunctions(DasLangImage &img)
    {
        img.funcInit        = img.ctx->findFunction("Init");
        img.funcUpdate      = img.ctx->findFunction("Update");
        img.funcDispatchMsg = img.ctx->findFunction("DispatchMsg");
    }

    void DasLangEngine::DoSwap(DasLangImage &&img)
    {
        _moduleProvider->DrainTimers();
        _scriptImage    = std::move(img);
        _lastGCHeapSize = 0;

        if (nullptr != _scriptFileWatcher)
        {
            _scriptFileWatcher->SetFiles(CollectDependencyFiles());
        }
    }

    void DasLangEngine::PollReload()
    {
        if (!_reloadPending.exchange(false, std::memory_order_acquire))
        {
            return;
        }

        std::string dasbin;
        {
            std::lock_guard lg(_reloadMutex);
            dasbin = _pendingDasbin;
        }

        DasLangImage img = Compile(_entryFile, dasbin);
        if (!img.program)
        {
            Log::Error("Reload compile failed, keep old imag:{}", img.errors);
            return;
        }

        if (!SimulateImage(img))
        {
            Log::Error("Reload simulate failed, keep old image:{}", img.errors);
            return;
        }

        RebindFunctions(img);

        if (nullptr != img.funcInit)
        {
            img.ctx->evalWithCatch(img.funcInit);
            if (auto ex = img.ctx->getException())
            {
                Log::Error("Rload Init exception:{}", ex);
            }
        }

        DoSwap(std::move(img));
        Log::Info("Script hot-reloaded");
    }

    std::vector<std::string> DasLangEngine::CollectDependencyFiles() const
    {
        std::vector<std::string> files;
        if (nullptr != _scriptImage.ctx)
        {
            for (auto *file : _scriptImage.ctx->getAllFiles())
            {
                if (nullptr != file && !file->name.empty())
                {
                    files.emplace_back(file->name);
                }
            }
        }

        return files;
    }

    das::CodeOfPolicies DasLangEngine::MakePolicies(EScriptMode mode, bool forAotGen)
    {
        das::CodeOfPolicies p;
        p.threadlock_context = true;
        p.persistent_heap    = true;
        p.rtti               = true;
        if (forAotGen)
        {
            p.aot                        = false;
            p.aot_module                 = true;
            p.fail_on_lack_of_aot_export = true;
        }
        else if (mode == EScriptMode::Release)
        {
            p.aot            = true;
            p.fail_on_no_aot = false; // 未命中回退解释器
        }
        else
        {
            p.debugger = true;
        }

        return p;
    }

    void DasLangEngine::BuildModuleGroup(DasLangImage &img)
    {
        img.moduleGroup = std::make_unique<das::ModuleGroup>();
        _moduleProvider->CreateModules(*img.moduleGroup);
    }

} // namespace MMO