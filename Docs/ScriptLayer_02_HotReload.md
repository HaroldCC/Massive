# 热重载（ScriptLayer_02_HotReload）

> 本篇给出 `DasLangEngine` 的**完整、可照抄**实现，接通「文件改动 → 重编译 → tick 之间换 Context」的整条链路，并修复此前审查发现的编译级阻断（F1 初始化顺序、F2 namespace 宏、F7 watcher 类型）。
>
> 承接 `ScriptLayer_00` / `01`。所有 daScript API 均对 `ThirdParty/daScript` 核实。

---

## 0. 现状与目标

审查确认：当前热重载「零件齐全、电路全断」——`DasFileWatcher` 从不 `Start`、`_reloadPending` 无人读、`DoSwap` 空实现、引擎从不被 `Initialize/Load/Tick`、`_scriptFileWatcher` 类型 `DasScriptWatcher` 根本不存在。本篇把它全部接通。

**热重载链路**：

```
文件改动(磁盘)
  └─ DasFileWatcher 轮询线程检测到 mtime/size 变化
       └─ 回调 RequestReloadFromSource()  →  置 _reloadPending
Tick(逻辑线程, 安全点):
  └─ 若 _reloadPending：
       Compile(entry) → SimulateImage(新 ctx) → RebindFunctions
       → DrainTimers(旧) → DoSwap(移入新 image) → eval Init → 刷新 watcher 文件集
```

**安全点**：`Context::restart()` 有 `DAS_ASSERTF(insideContext==0)`（`simulate.h:447`）——不能在 eval 执行中换。所以 swap 必须在 `Tick` 开头、任何 eval 之前。逻辑线程单线程驱动 tick/dispatch/timer，IO 线程不碰脚本，故 swap 无需锁（只有 `_reloadPending` 标志跨线程，用 atomic）。

---

## 1. 修复 F2：默认模块注册（namespace 安全）

`NEED_ALL_DEFAULT_MODULES` 在 `namespace MMO` 内展开会生成 `MMO::register_Module_*` 外部符号，链接失败（MinGW 实测复现）。改用 namespace 安全的 `DECLARE`/`PULL` 对：

**在文件作用域（namespace 之外）** 声明：

```cpp
// DasEngine.cpp 顶部，#include 之后，namespace MMO 之前
DECLARE_ALL_DEFAULT_MODULES;   // 文件作用域：声明全局 register_Module_* extern
```

**在 `Initialize()` 内**（namespace 里）拉起：

```cpp
PULL_ALL_DEFAULT_MODULES;      // :: 限定调用，namespace 内安全
```

二者定义见 `daScriptModule.h:72-96`。

---

## 2. `DasEngineConfig.h`（完整）

```cpp
#pragma once
#include "Common/Core/Types.h"
#include <string>

namespace MMO
{
    enum class EScriptMode
    {
        Develop, // 源码编译 + 热重载 + debugger
        Release, // AOT 基线 + .dabin 缓存/补丁
    };

    struct DasLangEngineConfig
    {
        std::string dasLangRoot   = "Script"; // 脚本根目录
        std::string entryFile     = "main.das"; // 入口脚本（相对 dasLangRoot）
        std::string dasbinDir     = "";       // .dabin 缓存目录（空=不缓存）
        std::string patchDir      = "";       // 热补丁目录
        std::string dabinKeyHex   = "";       // .dabin AES 密钥（hex，空=不加密）；见 03 篇
        EScriptMode mode          = EScriptMode::Develop;
        bool        enableWatcher = true;     // 开发期文件监视
        int64       watchPollMs   = 500;      // 监视轮询间隔
    };
} // namespace MMO
```

---

## 3. `DasImage.h`（完整）

```cpp
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

        // 缓存的入口函数（swap 后必须重取）
        das::SimFunction *funcInit        = nullptr;
        das::SimFunction *funcUpdate      = nullptr;
        das::SimFunction *funcDispatchMsg = nullptr;

        std::string errors;

        bool IsValid() const { return ctx != nullptr; }
    };
} // namespace MMO
```

---

## 4. `DasFileWatcher.h/.cpp`（完整）

用户已实现且 Stop/Loop 拆分正确。此处给出定稿版（含 swap 后刷新文件集的 `SetFiles`）。

```cpp
// DasFileWatcher.h
#pragma once
#include "Common/Core/Types.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace MMO
{
    class DasFileWatcher
    {
    public:
        using OnFileChanged = std::function<void()>;

        void Start(std::vector<std::string> files, int64 pollMs, OnFileChanged cb);
        void SetFiles(std::vector<std::string> files); // swap 后依赖集变化重建
        void Stop();

    private:
        void Loop();

        struct Snapshot { int64 mtime; int64 size; };

        std::unordered_map<std::string, Snapshot> _snapshot;
        std::mutex                                _mutex;
        std::jthread                              _thread;
        std::atomic<bool>                         _running {false};
        int64                                     _pollMs = 500;
        OnFileChanged                             _fileChangedCB;
    };
} // namespace MMO
```

```cpp
// DasFileWatcher.cpp
#include "DasFileWatcher.h"
#include "Common/Core/Types.h"
#include "Common/Log/Log.h"
#include <chrono>
#include <filesystem>
#include <system_error>

namespace MMO
{
    static bool StatFile(const std::string &path, int64 &mtime, int64 &size)
    {
        std::error_code ec;
        auto st = std::filesystem::last_write_time(path, ec);
        if (ec) { return false; }
        auto sz = std::filesystem::file_size(path, ec);
        if (ec) { return false; }
        mtime = static_cast<int64>(st.time_since_epoch().count());
        size  = static_cast<int64>(sz);
        return true;
    }

    void DasFileWatcher::Start(std::vector<std::string> files, int64 pollMs, OnFileChanged cb)
    {
        if (_running) { return; }
        _pollMs        = pollMs;
        _fileChangedCB = std::move(cb);
        SetFiles(std::move(files));
        _running.store(true);
        _thread = std::jthread([this] { Loop(); });
    }

    void DasFileWatcher::SetFiles(std::vector<std::string> files)
    {
        std::lock_guard lg(_mutex);
        _snapshot.clear();
        for (auto &f : files)
        {
            int64 m = 0, s = 0;
            if (StatFile(f, m, s)) { _snapshot[f] = {.mtime = m, .size = s}; }
        }
    }

    void DasFileWatcher::Stop()
    {
        _running.store(false);
        if (_thread.joinable()) { _thread.join(); }
    }

    void DasFileWatcher::Loop()
    {
        while (_running.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(_pollMs));
            bool changed = false;
            {
                std::lock_guard lg(_mutex);
                for (auto &[path, snap] : _snapshot)
                {
                    int64 m = 0, s = 0;
                    if (StatFile(path, m, s) && (m != snap.mtime || s != snap.size))
                    {
                        snap.mtime = m;
                        snap.size  = s;
                        changed    = true;
                    }
                }
            }
            if (changed && nullptr != _fileChangedCB) { _fileChangedCB(); }
        }
    }
} // namespace MMO
```

---

## 5. `DasEngine.h`（完整，修 F7）

关键修复：`_scriptFileWatcher` 类型从不存在的 `DasScriptWatcher` 改为 `DasFileWatcher`；显式声明析构以避免 `unique_ptr` 对不完整类型 delete。

```cpp
#pragma once
#include "Common/Core/Types.h"
#include "DasEngineConfig.h"
#include "DasImage.h"
#include "daScript/ast/ast.h"
#include "daScript/simulate/simulate.h"
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace MMO
{
    class IDasLangModuleProvider;
    class IDasLangHost;
    class DasCommonModule;
    class DasFileWatcher;   // ← 真实类型（修 F7：曾误写 DasScriptWatcher）

    class DasLangEngine
    {
    public:
        static DasLangEngine &GetIns();

        bool Initialize(const DasLangEngineConfig &cfg,
                        IDasLangHost              *host,
                        IDasLangModuleProvider    *moduleProvider);
        void Shutdown();

        bool Load(const std::string &entryFile);
        void Tick(float dt, uint32 sceneID = 0);

        // 热补丁：从 .dabin 重载（Release）
        void RequestReload(const std::string &dasbinPath);
        // 从源码重载（Develop，watcher 回调）
        void RequestReloadFromSource();

        das::Context *GetScriptContext() const { return _scriptImage.ctx.get(); }
        das::SimFunction *GetDispatchFunc() const { return _scriptImage.funcDispatchMsg; }
        const std::string &GetLastErrors() const { return _scriptImage.errors; }

    private:
        DasLangEngine();
        ~DasLangEngine();                       // 出 .cpp 外联（DasFileWatcher 完整类型可见）
        DasLangEngine(const DasLangEngine &) = delete;
        DasLangEngine &operator=(const DasLangEngine &) = delete;

        DasLangImage Compile(const std::string &entryFile, const std::string &dasbinPath);
        bool         SimulateImage(DasLangImage &img);
        void         RebindFunctions(DasLangImage &img);
        void         DoSwap(DasLangImage &&img);
        void         PollReload();              // Tick 开头调用，安全点执行 swap
        std::vector<std::string> CollectDependencyFiles() const;

        das::CodeOfPolicies MakePolicies() const;
        void                BuildModuleGroup(DasLangImage &img);

        DasLangEngineConfig               _cfg;
        IDasLangHost                     *_scriptHost     = nullptr;
        IDasLangModuleProvider           *_moduleProvider = nullptr;
        std::unique_ptr<DasCommonModule>  _commonModule;
        DasLangImage                      _scriptImage;
        std::string                       _entryFile;

        std::atomic<bool>                 _reloadPending {false};
        std::string                       _pendingDasbin;
        std::mutex                        _reloadMutex;

        std::unique_ptr<DasFileWatcher>   _scriptFileWatcher;
        uint64                            _lastGCHeapSize {0};
        bool                              _initialized = false;
    };
} // namespace MMO
```

---

## 6. `DasEngine.cpp`（完整，修 F1/F2 + 接通热重载）

```cpp
#include "DasEngine.h"
#include "DasEngineConfig.h"
#include "DasImage.h"
#include "DasSerializer.h"
#include "DasFileWatcher.h"
#include "IDasHost.h"
#include "IDasModuleProvider.h"
#include "Module/DasCommonModule.h"

#include "daScript/daScript.h"
#include "daScript/ast/ast.h"
#include "daScript/daScriptModule.h"
#include "daScript/simulate/fs_file_info.h"
#include "daScript/simulate/simulate.h"

#include "Common/Log/Log.h"

#include <atomic>
#include <memory>
#include <mutex>

// ★ F2 修复：默认模块声明放在文件作用域（namespace 之外），避免 namespace 污染符号
DECLARE_ALL_DEFAULT_MODULES;

namespace MMO
{
    DasLangEngine::DasLangEngine()  = default;
    DasLangEngine::~DasLangEngine() = default;   // 此处 DasFileWatcher / DasCommonModule 完整类型可见

    DasLangEngine &DasLangEngine::GetIns()
    {
        static DasLangEngine ins;
        return ins;
    }

    bool DasLangEngine::Initialize(const DasLangEngineConfig &cfg,
                                   IDasLangHost              *host,
                                   IDasLangModuleProvider    *moduleProvider)
    {
        if (_initialized) { return true; }

        _cfg            = cfg;
        _scriptHost     = host;
        _moduleProvider = moduleProvider;

        das::setDasRoot(cfg.dasLangRoot);

        // ★ F1 修复：必须先注册默认模块（含 '$' builtin），再构造/Build 自定义模块
        //   否则 DasCommonModule 构造 / Build().addBuiltInModule() 里 require("$") 返回 null → 断言崩溃
        PULL_ALL_DEFAULT_MODULES;              // namespace 安全的 NEED_ALL_DEFAULT_MODULES

        _commonModule = std::make_unique<DasCommonModule>();
        _commonModule->Build();

        das::Module::Initialize();             // finalize 所有模块

        _initialized = true;
        return true;
    }

    void DasLangEngine::Shutdown()
    {
        if (!_initialized) { return; }

        if (nullptr != _scriptFileWatcher) { _scriptFileWatcher->Stop(); }
        if (nullptr != _moduleProvider)    { _moduleProvider->DrainTimers(); }

        _scriptImage = DasLangImage {};        // 释放旧 Context/Program/ModuleGroup
        _commonModule.reset();

        das::Module::Shutdown();
        _initialized = false;
    }

    bool DasLangEngine::Load(const std::string &entryFile)
    {
        _entryFile = entryFile;

        // Release 优先试 .dabin；Develop 直接源码编译（dasbinPath 为空）
        std::string dasbinPath;
        if (_cfg.mode == EScriptMode::Release && !_cfg.dasbinDir.empty())
        {
            dasbinPath = _cfg.dasbinDir + "/" + entryFile + ".dabin";
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
        RebindFunctions(img);

        if (nullptr != img.funcInit)
        {
            img.ctx->evalWithCatch(img.funcInit);
            if (auto ex = img.ctx->getException())
            {
                Log::Error("Init exception:{}", ex);
            }
        }

        _scriptImage    = std::move(img);
        _lastGCHeapSize = 0;

        // Develop：启动文件监视（依赖集来自刚编译的 Context）
        if (_cfg.mode == EScriptMode::Develop && _cfg.enableWatcher)
        {
            if (nullptr == _scriptFileWatcher)
            {
                _scriptFileWatcher = std::make_unique<DasFileWatcher>();
            }
            _scriptFileWatcher->Start(CollectDependencyFiles(), _cfg.watchPollMs,
                                      [this] { RequestReloadFromSource(); });
        }
        return true;
    }

    void DasLangEngine::Tick(float dt, uint32 sceneID)
    {
        PollReload();                          // ★ 安全点：任何 eval 之前执行 swap

        if (!_scriptImage.IsValid()) { return; }

        _moduleProvider->OnPrevTick(dt);
        _scriptImage.ctx->restart();           // 每帧清 stopFlags/exception（非换 Context）

        if (nullptr != _scriptImage.funcUpdate)
        {
            vec4f args[] = {
                das::cast<uint32_t>::from(sceneID),
                das::cast<float>::from(dt),
            };
            _scriptImage.ctx->evalWithCatch(_scriptImage.funcUpdate, args);
            if (auto ex = _scriptImage.ctx->getException())
            {
                Log::Error("Update exception:{}", ex);
            }
        }

        uint64 heapNow = _scriptImage.ctx->heap->getTotalBytesAllocated();
        if (heapNow - _lastGCHeapSize > (4 * 1024 * 1024))
        {
            _scriptImage.ctx->collectHeap(nullptr, true, true);
            _lastGCHeapSize = _scriptImage.ctx->heap->getTotalBytesAllocated();
        }
    }

    void DasLangEngine::RequestReload(const std::string &dasbinPath)
    {
        std::lock_guard lg(_reloadMutex);
        _pendingDasbin = dasbinPath;
        _reloadPending.store(true, std::memory_order_release);
    }

    void DasLangEngine::RequestReloadFromSource()
    {
        std::lock_guard lg(_reloadMutex);
        _pendingDasbin.clear();
        _reloadPending.store(true, std::memory_order_release);
    }

    void DasLangEngine::PollReload()
    {
        if (!_reloadPending.exchange(false, std::memory_order_acquire)) { return; }

        std::string dasbin;
        {
            std::lock_guard lg(_reloadMutex);
            dasbin = _pendingDasbin;
        }

        DasLangImage img = Compile(_entryFile, dasbin);
        if (!img.program)
        {
            Log::Error("Reload compile failed, keep old image: {}", img.errors);
            return;                             // 安全网：编译失败保留旧脚本继续跑
        }
        if (!SimulateImage(img))
        {
            Log::Error("Reload simulate failed, keep old image: {}", img.errors);
            return;
        }
        RebindFunctions(img);

        if (nullptr != img.funcInit)
        {
            img.ctx->evalWithCatch(img.funcInit);
            if (auto ex = img.ctx->getException())
            {
                Log::Error("Reload Init exception:{}", ex);
            }
        }

        DoSwap(std::move(img));
        Log::Info("script hot-reloaded");
    }

    DasLangImage DasLangEngine::Compile(const std::string &entryFile, const std::string &dasbinPath)
    {
        DasLangImage img;
        BuildModuleGroup(img);

        img.fileAccess = das::make_smart<das::FsFileAccess>();
        static_cast<das::FsFileAccess *>(img.fileAccess.get())->introduceDaslib();

        // 先试 .dabin 缓存/补丁（见 03 篇 DasLangSerializer）
        if (!dasbinPath.empty())
        {
            std::string err;
            auto program = DasLangSerializer::Load(dasbinPath, *img.moduleGroup,
                                                   img.fileAccess.get(), _cfg.dabinKeyHex, err);
            if (program)
            {
                img.program = program;
                return img;
            }
            Log::Info("dabin miss ({}), fall back to full compile", err);
            BuildModuleGroup(img);              // Load 失败可能弄脏 library，重建
        }

        das::TextWriter logs;
        img.program = das::compileDaScript(entryFile, img.fileAccess, logs,
                                           *img.moduleGroup, MakePolicies());
        if (!img.program || img.program->failed())
        {
            img.errors  = logs.str();
            img.program = nullptr;
        }
        return img;
    }

    bool DasLangEngine::SimulateImage(DasLangImage &img)
    {
        img.ctx = std::make_shared<das::Context>(img.program->getContextStackSize());
        _moduleProvider->OnContextSwapped(img.ctx);   // 让 provider 把新 ctx 绑到各模块

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
        img.funcDispatchMsg = img.ctx->findFunction("dispatch_msg");  // ★ 精确名（非 DispatchMsg）
    }

    void DasLangEngine::DoSwap(DasLangImage &&img)
    {
        _moduleProvider->DrainTimers();         // 清旧 Context 的定时器回调（防悬空）
        _scriptImage    = std::move(img);       // 移入新 image；旧 ctx 在此析构
        _lastGCHeapSize = 0;

        // 依赖集可能变化（新增 require），刷新监视清单
        if (nullptr != _scriptFileWatcher)
        {
            _scriptFileWatcher->SetFiles(CollectDependencyFiles());
        }
    }

    std::vector<std::string> DasLangEngine::CollectDependencyFiles() const
    {
        std::vector<std::string> files;
        if (_scriptImage.ctx)
        {
            for (auto *fi : _scriptImage.ctx->getAllFiles())   // simulate.h:768
            {
                if (fi && fi->name != nullptr) { files.emplace_back(fi->name); }
            }
        }
        return files;
    }

    das::CodeOfPolicies DasLangEngine::MakePolicies() const
    {
        das::CodeOfPolicies p;
        p.threadlock_context = true;
        p.persistent_heap    = true;
        p.rtti               = true;
        if (_cfg.mode == EScriptMode::Develop)
        {
            p.debugger = true;
        }
        else
        {
            p.aot            = true;
            p.fail_on_no_aot = false;           // 缺 AOT 回退解释器（见 04 篇）
        }
        return p;
    }

    void DasLangEngine::BuildModuleGroup(DasLangImage &img)
    {
        img.moduleGroup = std::make_unique<das::ModuleGroup>();
        _moduleProvider->CreateModules(*img.moduleGroup);
    }

} // namespace MMO
```

---

## 7. 关键点说明

- **F1 顺序**：`PULL_ALL_DEFAULT_MODULES` → 构造 `DasCommonModule` + `Build()` → `das::Module::Initialize()`。三步不可乱。`Build()` 里 `addBuiltInModule()` 依赖 `$` 已注册。
- **swap 安全点**：`PollReload()` 放在 `Tick` 最前、`restart()`/`eval` 之前。此时 `insideContext==0`，新旧 Context 短暂并存，`std::move` 后旧的析构。逻辑线程单线程，无需锁保护 image。
- **DrainTimers 时机**：`DoSwap` 开头调用——旧 Context 的定时器回调持有旧 ctx 的 shared_ptr，不清会 eval 悬空堆。`OnContextSwapped` 已在 `SimulateImage` 里对新 ctx 调过，`DoSwap` 不重复。
- **编译失败安全网**：`PollReload` 里编译/simulate 失败**保留旧 image** 继续跑，只记错误——热重载写错脚本不会打挂服务器。
- **依赖集刷新**：`getAllFiles()` 返回该 Context 的全部源文件（含所有 `require` 依赖），swap 后重新喂给 watcher，新增 `require` 的文件也纳入监视。
- **`Tick` 签名加 `sceneID`**：修此前只传 `dt` 的 arity 错位（`Update(sceneID:uint, dt:float)` 要两参，eval 不校验 arity 会静默错栈）。

---

## 8. 状态迁移（换 Context 会丢的东西）

换 Context 后，脚本全局变量（存活于旧 Context general heap）全部丢失、新 Context 归零。但本项目的关键全局是**自动重建**的：

- `g_handler_registry`：由 `[msg_handler]` 的 `[init]` 注入代码在新 Context `simulate` 时重新填充——无需手搬。
- 业务全局（若有需要跨 reload 保留的状态）：daScript 提供 `[before_reload]`/`[after_reload]` 或 `@live` 变量宏（`daslib/live`）。本轮 handler 无状态，暂不需要。后续业务若有跨 reload 状态，参照 `skills/daslang_live.md`。

失效必须重取的（`RebindFunctions` + provider 已覆盖）：所有 `SimFunction*`（Init/Update/dispatch_msg）、provider 内绑定到 ctx 的引用（经 `OnContextSwapped`）、定时器（经 `DrainTimers` 清除）。

---

## 9. 验证步骤

1. Develop 模式启动，`Init` 打印日志，watcher 线程起。
2. 改 `Script/Handlers.das` 里 `handle_move` 的日志文案，保存。
3. 下一 tick 内 `PollReload` 检测到变化 → 重编译 → swap，日志出现 `script hot-reloaded`。
4. 再发 `MoveReq`，`handle_move` 用**新**文案响应，且服务未重启、连接未断。
5. 故意写一个语法错误保存 → 日志出现 `Reload compile failed, keep old image`，服务继续用旧脚本正常跑。
