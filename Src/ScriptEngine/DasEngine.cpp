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
#include "daScript/das_project_specific.h"
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

// 文件作用域声明 register_DasCommonModule()（PULL_MODULE 需要）
DECLARE_MODULE(DasCommonModule);

// daslang 库提供的 get_file_access（ast_parse.cpp）：pak 非空时编译 project 文件返回带回调的 access
DAS_CC_API das::FileAccessPtr get_file_access(char *pak);

namespace
{
    /**
     * @brief 项目版 get_file_access（经 set_project_specific_fs_callbacks 注册）
     *
     * 官方 get_file_access 在编译 .das_project 时用默认 FsFileAccess（未 introduceDaslib），
     * 导致 project 文件里的 `require daslib/xxx` 解析失败。项目版先引入 daslib 再编译，
     * 使 .das_project 的 module_get/module_allowed 等回调正常加载。
     */
    das::FileAccessPtr ProjectGetFileAccess(char *pak)
    {
        if (nullptr != pak && *pak)
        {
            auto inner = das::make_smart<das::FsFileAccess>();
            static_cast<das::FsFileAccess *>(inner.get())->introduceDaslib();
            das::ModuleGroup dummy;
            das::TextWriter  tout;
            auto             program = das::compileDaScript(pak, inner, tout, dummy);
            if (program && !program->failed())
            {
                return das::make_smart<das::FsFileAccess>(pak, program);
            }
            // project 编译失败：回退默认 access（含 daslib），脚本编译仍可进行
            MMO::Log::Error("DasEngine: .das_project compile failed: {}", tout.str());
            return inner;
        }
        auto fa = das::make_smart<das::FsFileAccess>();
        static_cast<das::FsFileAccess *>(fa.get())->introduceDaslib();
        return fa;
    }
} // namespace

namespace MMO
{
    namespace
    {
        // 注册表单例：引擎实例由宿主（WorldServer）在 Initialize 前显式注册，
        // 替代 Meyers static 局部单例（隐式全局状态、初始化顺序不可控）。
        // 生成代码/Bridge 经 GetIns() 访问已注册实例；Initialize 会 SetInstance。
        DasLangEngine *g_dasEngine = nullptr;
    } // namespace

    DasLangEngine &DasLangEngine::GetIns()
    {
        // 已注册则返回实例；否则提供默认实例（兼容未显式注册的调用路径）
        if (nullptr != g_dasEngine)
        {
            return *g_dasEngine;
        }
        static DasLangEngine defaultInstance;
        return defaultInstance;
    }

    void DasLangEngine::SetInstance(DasLangEngine &engine)
    {
        g_dasEngine = &engine;
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

        // 注册项目版 get_file_access——使 .das_project 编译时能解析 daslib（官方默认 access 缺 daslib）
        das::set_project_specific_fs_callbacks(&ProjectGetFileAccess);

        // 官方范式：PULL 默认模块 → PULL 服务模块（Common/world 经 REGISTER_MODULE 自注册，
        // 构造函数注册绑定）→ Module::Initialize() 统一处理。
        // 模块实例归 daScriptEnvironment 全局链表管理，das::Module::Shutdown() 统一释放——
        // 不再 unique_ptr 持有，消除 double-delete。
        PULL_ALL_DEFAULT_MODULES;

        // Common 模块（引擎级公共绑定：Log + EMsgID）——DECLARE_MODULE 在文件作用域声明
        PULL_MODULE(DasCommonModule);
        // 服务专用模块（world/social 等）——provider 在 PULL_ALL_DEFAULT_MODULES 之后
        // 触发 PULL_MODULE(WorldScriptModule)，保证其构造函数里 addBuiltInModule() 可用。
        if (nullptr != _moduleProvider)
        {
            _moduleProvider->RegisterModules();
        }

        das::Module::Initialize();

        // 保存已初始化环境——Module::Initialize() 只在当前线程（主线程）的
        // thread-local daScriptEnvironment 置 g_modulesInitialized=true。
        // 热重载的 PollReload 在 LogicThread 执行，那里的环境未初始化，
        // compileDaScript 的断言会失败，需在此保存的环境上建 guard 临时绑定。
        _env = das::daScriptEnvironment::getBound();

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

        // 模块实例归 daScriptEnvironment 全局链表管理，das::Module::Shutdown() 统一释放。
        // 不再手动 reset（消除 double-delete）。
        das::Module::Shutdown();
        _initialized = false;
    }

    bool DasLangEngine::Load(const std::string &entryFile)
    {
        _entryFile = entryFile;

        // Release 优先尝试 .dasbin Develop直接源码编译。
        // dabin 文件名用入口 basename（去目录）：Script/World/main.das → main.dasbin
        std::string dasbinPath = "";
        if (_cfg.mode == EScriptMode::Release)
        {
            std::string base  = entryFile;
            auto        slash = base.find_last_of("/\\");
            if (slash != std::string::npos)
            {
                base = base.substr(slash + 1);
            }
            // 补丁 dasbin（patchDir）优先于常规缓存（dasbinDir）——发布期热补丁路径
            if (!_cfg.patchDir.empty())
            {
                dasbinPath = std::format("{}/{}.dasbin", _cfg.patchDir, base);
            }
            else if (!_cfg.dasbinDir.empty())
            {
                dasbinPath = std::format("{}/{}.dasbin", _cfg.dasbinDir, base);
            }
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

        // save dasbin —— 仅当本次是从源码全量编译（未命中 dabin）时生成缓存
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

            std::string base  = entryFile;
            auto        slash = base.find_last_of("/\\");
            if (slash != std::string::npos)
            {
                base = base.substr(slash + 1);
            }
            std::string outPath = std::format("{}/{}.dasbin", _cfg.dasbinDir, base);
            DasLangSerializer::Save(outPath, img.program, *img.moduleGroup, depFiles, _cfg.dasbinKeyHex);
        }

        RebindFunctions(img);

        // Release 模式 AOT 覆盖率诊断——统计脚本函数中 AOT 命中的比例。
        // fail_on_no_aot=false 会让 aotHash 未命中的函数静默回退解释器，无任何报错；
        // 此日志让"AOT 全败"不再无感（若命中率异常低说明生成端/运行端 aotHash 漂移）。
        if (_cfg.mode == EScriptMode::Release)
        {
            LogAotCoverage(img);
        }

        CallScriptFunctionIn(img.ctx.get(), img.funcInit, "Init");

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

        // 脚本 Update 当前签名 ()（0 参，见 Script/World/main.das）。
        // CallScriptFunction 在调用前校验 arity——签名漂移立即报错而非静默错栈。
        CallScriptFunction(_scriptImage.funcUpdate, "Update");

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
        // 热重载在 LogicThread 执行，必须临时绑定 Initialize 时保存的已初始化环境
        return WithDasEnvironment([&]() -> DasLangImage {
            DasLangImage img;
            BuildModuleGroup(img);

            // 启用 .das_project：get_file_access(pak) 由 daslang 库提供，pak 非空时
            // 编译并 simulate project 文件（提取 module_get/module_allowed 等回调），
            // 返回带 project 解析规则的 FileAccess。为空时返回默认 FsFileAccess。
            img.fileAccess = ::get_file_access(const_cast<char *>(_cfg.projectFile.c_str()));
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
            // ⚠ compileDaScript 内部可能 throw dasException（解析/语义错误、模块缺失等），
            // 必须在引擎边界捕获——否则异常逃出 PollReload → Tick → OnTick → LogicThread，
            // 整个逻辑线程崩溃（热重载触发编译错误的场景）。
            das::TextWriter logs;
            try
            {
                img.program = das::compileDaScript(enteryFile,
                                                   img.fileAccess,
                                                   logs,
                                                   *img.moduleGroup,
                                                   MakePolicies(_cfg.mode, false));
            }
            catch (const std::exception &e)
            {
                img.errors  = std::string("compileDaScript threw: ") + e.what();
                img.program = nullptr;
                Log::Error("Compile [{}] exception:{}", enteryFile, img.errors);
            }
            if (!img.program || img.program->failed())
            {
                img.errors = logs.str();
                // program->errors 可能含更详细错误（logs 可能为空）
                if (img.program)
                {
                    for (const auto &perr : img.program->errors)
                    {
                        if (!img.errors.empty())
                        {
                            img.errors += "\n";
                        }
                        img.errors += perr.what;
                    }
                }
                Log::Error("Compile [{}] failed:{}", enteryFile, img.errors);
                img.program = nullptr;
            }

            return img;
        });
    }

    bool DasLangEngine::SimulateImage(DasLangImage &img)
    {
        // simulate 同样依赖已初始化的 daScript 环境（模块解析/绑定），
        // 热重载时在 LogicThread 执行，需临时绑定。
        return WithDasEnvironment([&]() -> bool {
            img.ctx = std::make_shared<das::Context>(img.program->getContextStackSize());
            _moduleProvider->OnContextSwapped(img.ctx);

            das::TextWriter logs;
            try
            {
                if (!img.program->simulate(*img.ctx, logs))
                {
                    img.errors = logs.str();
                    img.ctx    = nullptr;
                    return false;
                }
            }
            catch (const std::exception &e)
            {
                img.errors = std::string("simulate threw: ") + e.what();
                img.ctx    = nullptr;
                return false;
            }

            return true;
        });
    }

    void DasLangEngine::RebindFunctions(DasLangImage &img)
    {
        img.funcInit        = img.ctx->findFunction("Init");
        img.funcUpdate      = img.ctx->findFunction("Update");
        img.funcDispatchMsg = img.ctx->findFunction("DispatchMsg");

        // 诊断：确认脚本侧入口函数都绑定成功（DispatchMsg 由 MsgHandlerRegistry 模块导出）
        Log::Info("DasEngine: RebindFunctions Init={} Update={} DispatchMsg={}",
                  (nullptr != img.funcInit) ? "ok" : "MISS",
                  (nullptr != img.funcUpdate) ? "ok" : "MISS",
                  (nullptr != img.funcDispatchMsg) ? "ok" : "MISS");
    }

    void DasLangEngine::LogAotCoverage(const DasLangImage &img) const
    {
        if (nullptr == img.ctx)
        {
            return;
        }

        int32_t                  total    = img.ctx->getTotalFunctions();
        int32_t                  aotCount = 0;
        int32_t                  exported = 0;
        std::vector<std::string> missed;
        for (int32_t i = 0; i < total; ++i)
        {
            auto *fn = img.ctx->getFunction(i);
            if (nullptr == fn)
            {
                continue;
            }
            // 只统计脚本导出/可调用函数（跳过 builtin——builtin 无 AOT）
            if (nullptr != fn->debugInfo && (fn->debugInfo->flags & das::FuncInfo::flag_builtin))
            {
                continue;
            }
            ++exported;
            if (fn->aot)
            {
                ++aotCount;
            }
            else
            {
                missed.emplace_back(nullptr != fn->name ? fn->name : "?");
            }
        }

        if (exported > 0)
        {
            const float ratio = 100.0f * static_cast<float>(aotCount) / static_cast<float>(exported);
            Log::Info("DasEngine: AOT coverage {}/{} functions ({:.1f}%)", aotCount, exported, ratio);
            if (!missed.empty())
            {
                std::string names;
                for (size_t k = 0; k < missed.size(); ++k)
                {
                    if (k > 0)
                    {
                        names += ", ";
                    }
                    names += missed[k];
                }
                Log::Info("DasEngine: non-AOT functions: {}", names);
            }
            if (aotCount == 0)
            {
                Log::Error("DasEngine: AOT coverage is 0% — aotHash mismatch? "
                           "Check AotGen generated .das.cpp is linked & policies match");
            }
        }
        else
        {
            Log::Info("DasEngine: AOT coverage skipped (no exported functions)");
        }
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

        // 官方 daslang-live 协议：旧 Context 销毁前 BeforeReload（保存跨重载状态），
        // DoSwap 替换整图，新 Context [init] 重建脚本全局后 AfterReload（恢复状态）。
        if (nullptr != _moduleProvider)
        {
            _moduleProvider->BeforeReload(_scriptImage.ctx);
        }

        RebindFunctions(img);

        CallScriptFunctionIn(img.ctx.get(), img.funcInit, "Reload Init");

        DoSwap(std::move(img));

        if (nullptr != _moduleProvider)
        {
            _moduleProvider->AfterReload(_scriptImage.ctx);
        }
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
        p.version_2_syntax   = true; // 项目脚本统一 v2 语法（main.das / daslib 均 gen2）
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
        // 公共模块：Common（日志等全服务通用绑定）——经 REGISTER_MODULE 自注册进全局链表，
        // 这里用 Module::require 获取实例。模块生命周期归 das::Module::Shutdown 统一管理。
        auto *commonModule = das::Module::require("Common");
        if (nullptr != commonModule)
        {
            img.moduleGroup->addModule(commonModule);
        }
        else
        {
            Log::Error("DasEngine: Common module not registered — PULL_MODULE(DasCommonModule) missing?");
        }
        // 服务专用模块（world/social 等）：消息类型 + EMsgID + 业务绑定
        // provider 用 Module::require("world") 复用已 PULL 的实例——
        // 不 new（避免 DAS_FATAL_ERROR）不持有（避免 double-delete）。
        _moduleProvider->CreateModules(*img.moduleGroup);
    }

} // namespace MMO