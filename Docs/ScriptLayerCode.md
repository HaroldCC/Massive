# Massive 脚本层 — 可逐行照抄实现手册

> 本文是 `ScriptLayerImplementation.md`（设计级）的**编码级配套**。目标：你能对照本文逐行敲出可编译代码。所有 daScript API、旧 `MassiveModule` 细节、`WorldServer`/`gen.cpp` 现状均已核实。
>
> **读法**：按 §A→§B→…→§F 顺序落地（= 设计文档的阶段 0→4）。每节给出完整文件内容或精确 diff。标 `★` 处是易错点。

---

## 硬约束回顾（务必先读）

1. `.dabin` 读写必须用 `AstSerializer::serializeProgram(program, libGroup)`，读取端设 `deser.thisModuleGroup = &libGroup`，libGroup 先加载全部 live native 模块。**不可用裸 C API**（native 函数指针不 round-trip）。
2. `.dabin` 是编译后 AST（含 `aotHash`），不含源码明文。
3. 反序列化出的是 `Program`，仍需 `simulate()` 得 `Context`。
4. `.dabin` 按服务隔离：反序列化端 libGroup 必须含该服务全部 live 模块（World=Common+World），否则 `failToCompile`。
5. DasPack 生成端与运行端的 `CodeOfPolicies`、daslang 版本、native 模块集合必须逐一对齐。

---

## §0 本地代码审查修正（已按文档实现，以下为必修项）

你已搭好 ScriptEngine 骨架并修复了首轮多数问题。以下按**当前状态**分两组。

**✅ 已修复（首轮 B1-B7）**：B1 `RequestReload` 加了 `DasLangEngine::` 前缀；B2 `SimulateImage` 失败已 `return false`；B4 `DasSerializer` 用 `ByteBuffer` 补全 + `WriteWholeFile` 返回值已修；B5 `DasFileWatcher.h/.cpp` 已建（Stop/Loop 拆分正确）；B7 config 已重命名。xmake 也补了 `CommonLog`/`CommonTimer`。

**❗ 仍需修正（第二轮，含 workflow 对抗验证过的硬结论）**：

| # | 位置 | 问题 | 修正 |
|---|---|---|---|
| **C1 崩溃** | `DasEngine::Initialize` | 缺 `NEED_ALL_DEFAULT_MODULES`。`CommonModule::BindFunctions` 里 `lib.addBuiltInModule()` → `Module::require("$")` 会 `DAS_ASSERTF` **断言崩溃**（`ast_module.cpp:936`）。旧代码 git `1d28cdcf` 有此宏 | `Module::Initialize()` **之前**加 `NEED_ALL_DEFAULT_MODULES;`。顺序：NEED 宏 → 构造模块+BindFunctions → `Module::Initialize()`（见 §A.8 已更新） |
| **C2 分发失效** | `DasEngine::RebindFunctions` `findFunction("DispatchMsg")` | 脚本导出的是 `def dispatch_msg`（小写下划线，`HandlerRegistry.das:104`）。findFunction **精确大小写匹配** → 返回 null → 分发恒失效 | 改 `findFunction("dispatch_msg")`（`Init`/`Update` 名字正确✓） |
| **C3 错栈** | `DasEngine::Tick` `args[]` 只传 `dt` | `Update(sceneID:uint, dt:float)` 要 2 个参数。eval **无 arity 校验** → 静默错栈（sceneID 读到 dt 的 bits，dt 读到栈垃圾） | `vec4f args[2] = { das::cast<uint32>::from(sceneID), das::cast<float>::from(dt) }` |
| **C4 分层** | `DasEngine.cpp` `#include "World/AutoGen/ProtoBindIndex.gen.h"` + `RegisterAllMsgDispatch()` | ScriptEngine 反向依赖 World，xmake 无 World dep → 链接缺符号 + 破坏分层（B6 未改） | dispatch 注册移出引擎，交 World 侧 Load 后调（见 §0.1） |
| **C5 空实现** | `DasEngine::DispatchMsg` | 判空后直接 `return true`，没 eval | 要么实现 `ctx->eval(funcDispatchMsg, args)`；要么删（实际分发走 gen.cpp 的 `ScriptDispatchRegistry`，此方法可能多余） |
| **C6 GC bug** | `DasHelpers::CreateDasArrayFromVector` | `TArray` 基类 `Array` 的 `size`/`capacity` 是 **`uint64_t` 非 uint32**（`arraytype.h:156-157`），`(uint32_t)` cast 截断；手设 `data/size/capacity` 不设 `magic`/`lock` → GC / `array_reserve` 会 realloc/free `arr.data`。`try/catch` 也无意义（`ctx->allocate` 不抛） | 用 `array_mark_locked(arr, data, size, cap)`（`runtime_array.cpp:12`，设 `lock=1`+`magic`）；或整体重写/暂不用 |
| **C7 可移植** | `DasSerializer::Save` `buf.WriteBytes(&h, sizeof(h))` | `DasbinHeader` 结构体整块 memcpy → 含 padding + 主机字节序，与后面 `ByteBuffer` 大端逐字段写的 dep 表**不一致**，跨编译器/平台不可移植 | header 也**逐字段** `WriteUint32`（magic 用 `WriteBytes`），全程走 ByteBuffer 大端 |
| C8 缓存缺 | `DasEngine::Load` | `dasbinPath` 恒 `""`，只有 Load 分支没 Save 分支 → `.dasbin` 永不生成 | simulate 成功后收集 `getAllFiles` 依赖 + `DasLangSerializer::Save`（见 §B.4，属阶段1收尾） |
| C9 死成员 | `DasCommonModule` `_host`/`_timingWheel` | 声明了但 `Build()` 没用、无注入 | 删除；Log 桥接不需要 ctx/host（`Log::At` 自带 LineInfo） |
| C10 拼写 | `DasEngine::Compile` 参数 `enteryFile` | 拼写 | → `entryFile` |

> **验证澄清（无需改）**：①模块构造+`Build()` 在 `Module::Initialize()` **之前**是**正确**的（daScript 惯例，模块在构造函数里注册进环境链表）。②`_commonModule` **不必** `addModule` 进 group——native 模块经环境全局表解析 `require`，编译时自动加进 libGroup（`ast_parse.cpp:504`）。所以 §A.6 provider 里 `group.addModule(Common())` 那句其实**多余**（无害），可省；provider 只需加服务专用模块。

### §0.1 分层修正：dispatch 注册移出引擎（B6）

`DasLangEngine::Load` 现在直接调了 `RegisterAllMsgDispatch()`（World 的 gen 函数），这让下层引擎依赖了上层 World。正确做法：**引擎只负责编译/simulate/缓存函数指针，dispatch 注册是 World 的职责**。

- 引擎 `Load()` 删掉 `#include "World/.../ProtoBindIndex.gen.h"` 和 `RegisterAllMsgDispatch()`。
- World 侧（`WorldServer::Init`）在 `DasLangEngine::GetIns().Load(...)` 成功**之后**自己调 `RegisterAllMsgDispatch()`。
- 若担心热重载后 dispatch 表失效：`ScriptDispatchRegistry` 存的是 C++ 静态函数指针，本身不随 Context 变，注册一次即可；它们运行时经 `GetScriptContext()`/`GetDispatchFunc()` 取当前 Context，swap 后自动跟随，无需重注册。

> 这条即使你还没设计 World/Social，也应先做——它是"ScriptEngine 是下层公共库"的边界纪律。引擎的公共 API 里不能出现任何 `World/` 符号。

---

# §A 阶段 0 — 拆分模块 + 收敛到 DasLangEngine（前置，修复编译）

拆分依据（已核实旧 `MassiveModule`）：

| 原函数 | 归属 | 理由 |
|---|---|---|
| `LogInfo/LogWarn/LogError` | **CommonModule** | 纯日志，跨服务通用 |
| `EntityPosition/IsDead/IsInCombat/IsStunned/IsPlayer/IsMonster/CreateEntity/DestroyEntity/GetBattleStats/EntitiesInRadius` | **WorldModule** | 依赖 Scene/EnTT/Component |
| `SendToClient/FindEntityBySession` | **WorldModule** | 依赖 WorldServer/WorldSession |
| `ScheduleTimer/CancelTimer/GetDeltaTime` | **WorldModule**（定时器状态） | 依赖 TimingWheel；`_scriptDt`/`_timerCallbacks` 随之 |
| `RegisterCommonProtoBindings`（公共 Proto） | **CommonModule** | Login/Common 消息跨服务 |
| `RegisterLogin/MoveProtoBindings`（World 专属 Proto） | **WorldModule** | World 消息 |

> 定时器状态（`_scriptDt`/`_timerCallbacks`/`_nextTimerID`）跟着 `ScheduleTimer` 走，落在 WorldModule。CommonModule 只留无状态的 Log。

## A.1 `Src/ScriptEngine/IScriptModuleProvider.h`（新建）

```cpp
#pragma once

#include "daScript/ast/ast.h"

#include <memory>

namespace MMO
{
    // 服务（World/Social）向引擎提供其 native 模块集与入口脚本。
    // 引擎服务无关，只通过此接口拿模块。
    class IScriptModuleProvider
    {
    public:
        virtual ~IScriptModuleProvider() = default;

        // 把本服务所需的全部 live native 模块 addModule 进 group（含公共 CommonModule）。
        // 每次编译 / 反序列化都会调用——它决定 libGroup 内容（硬约束 1/4）。
        virtual void CreateModules(das::ModuleGroup &group) = 0;

        // swap 后，引擎把新 Context 通知给各专用模块（重绑 ctx）。
        virtual void OnContextSwapped(std::shared_ptr<das::Context> ctx) = 0;

        // 每帧 Tick 前，引擎把 dt 转发给需要它的专用模块（如 WorldModule::_scriptDt）。
        virtual void OnPreTick(float dt) = 0;

        // swap 前，引擎要求各专用模块清空绑旧 Context 的定时器回调。
        virtual void DrainTimers() = 0;

        virtual const char *EntryScript() const = 0; // 如 "World/ServerTick.das"
        virtual const char *ServiceName() const = 0; // 如 "world"
    };
} // namespace MMO
```

## A.2 `Src/ScriptEngine/Module/CommonModule.h`（新建，原 DasCommonModule 改造）

```cpp
#pragma once

#include "ScriptEngine/IDasHost.h"
#include "daScript/ast/ast.h"
#include "daScript/daScriptModule.h"
#include "daScript/simulate/simulate.h"

#include <memory>

namespace MMO
{
    // 公共 native 模块 "common"：Log + 公共 Proto 绑定。无状态（Log 不需 ctx）。
    class CommonModule : public das::Module
    {
    public:
        CommonModule();

        void BindFunctions();

    private:
        // Log 桥接函数在 .cpp 内 static
    };
} // namespace MMO
```

## A.3 `Src/ScriptEngine/Module/CommonModule.cpp`（新建）

```cpp
#include "ScriptEngine/Module/CommonModule.h"
#include "Common/Log/Log.h"
#include "World/AutoGen/ProtoBindIndex.gen.h" // 公共 Proto 注册入口（见 A.9 拆分）

#include "daScript/ast/ast_interop.h"
#include "daScript/simulate/simulate.h"

using namespace das;

namespace MMO
{
    // ── 日志桥接（签名固定：text + Context* + LineInfoArg*）──
    static void Common_LogInfo(const char *text, das::Context * /*ctx*/, das::LineInfoArg *at)
    {
        if (at && at->fileInfo)
            Log::At(ELogLevel::Info, at->fileInfo->name.c_str(), (int)at->line, "{}", text ? text : "(null)");
        else
            Log::Info("{}", text ? text : "(null)");
    }
    static void Common_LogWarn(const char *text, das::Context * /*ctx*/, das::LineInfoArg *at)
    {
        if (at && at->fileInfo)
            Log::At(ELogLevel::Warn, at->fileInfo->name.c_str(), (int)at->line, "{}", text ? text : "(null)");
        else
            Log::Warn("{}", text ? text : "(null)");
    }
    static void Common_LogError(const char *text, das::Context * /*ctx*/, das::LineInfoArg *at)
    {
        if (at && at->fileInfo)
            Log::At(ELogLevel::Error, at->fileInfo->name.c_str(), (int)at->line, "{}", text ? text : "(null)");
        else
            Log::Error("{}", text ? text : "(null)");
    }

    CommonModule::CommonModule()
        : Module("common")
    {
    }

    void CommonModule::BindFunctions()
    {
        ModuleLibrary lib(this);
        lib.addBuiltInModule();
        auto *builtin = Module::require("$");
        if (builtin)
        {
            addBuiltinDependency(lib, builtin, true);
        }

        addExtern<DAS_BIND_FUN(Common_LogInfo)>(*this, lib, "LogInfo", SideEffects::modifyExternal)
            ->args({"text", "context", "at"});
        addExtern<DAS_BIND_FUN(Common_LogWarn)>(*this, lib, "LogWarn", SideEffects::modifyExternal)
            ->args({"text", "context", "at"});
        addExtern<DAS_BIND_FUN(Common_LogError)>(*this, lib, "LogError", SideEffects::modifyExternal)
            ->args({"text", "context", "at"});

        // 公共 Proto 消息类型（Login/Common），见 A.9 的 RegisterCommonMessageTypes
        RegisterCommonMessageTypes(*this, lib);
    }

    // ★ 不要在 CommonModule 写 REGISTER_DYN_MODULE/REGISTER_MODULE。
    //   这些宏用于"动态可发现模块"（daslang.exe 按名加载）。我们的模块由 C++ 直接
    //   new + addModule，不走动态发现。原 DasCommonModule 的那两行删掉。
} // namespace MMO
```

> **★ 关于 `REGISTER_DYN_MODULE`/`REGISTER_MODULE`**：旧 `MassiveModule`/`DasCommonModule` 有这两行。它们注册的是"可被 daslang 按名动态加载的模块"。你的引擎是 C++ 端 `new CommonModule()` 再 `addModule`，**不需要**动态发现，删除这两行可避免与 `NEED_MODULE` 机制耦合。（若你确实要走 `NEED_MODULE(CommonModule)` + `Module::Initialize` 的自动注册路径，则保留，但那样 provider 里就不能再自己 new——二选一，本文档走"C++ 直接 new"路线。）

## A.4 `Src/World/Script/WorldModule.h`（新建，承接 15 个 World 桥接函数）

```cpp
#pragma once

#include "Common/Core/Types.h"
#include "daScript/ast/ast.h"
#include "daScript/daScriptModule.h"
#include "daScript/simulate/simulate.h"

#include <atomic>
#include <memory>
#include <unordered_map>

namespace MMO
{
    class WorldServer;
    class SceneManager;
    class TimingWheel;
    struct WorldSession;

    // World 专用 native 模块 "world"：Scene/EnTT/战斗 + 定时器 + World 专属 Proto。
    class WorldModule : public das::Module
    {
    public:
        WorldModule(WorldServer                              *worldServer,
                    SceneManager                             *sceneMgr,
                    TimingWheel                              *timingWheel,
                    std::unordered_map<uint32, WorldSession> *sessions);
        ~WorldModule() override;

        void BindFunctions();

        // ── swap 生命周期（供 provider 调用）──
        void SetContext(std::shared_ptr<das::Context> ctx) { _ctx = ctx; }
        void SetScriptDt(float dt) { _scriptDt.store(dt, std::memory_order_relaxed); }
        void DrainTimers();
        das::Context *GetContext() const { return _ctx.get(); }

        // ── static 桥接函数经全局指针访问（Init/Tick 只读，无并发）──
        WorldServer                              *_worldServer = nullptr;
        SceneManager                             *_sceneMgr    = nullptr;
        TimingWheel                              *_timingWheel = nullptr;
        std::unordered_map<uint32, WorldSession> *_sessions    = nullptr;

        std::shared_ptr<das::Context> _ctx;
        std::atomic<float>            _scriptDt {0.02f};

        struct TimerCallback
        {
            das::TBlock<void, uint32_t>   block;
            std::shared_ptr<das::Context> ctx;
        };
        std::unordered_map<uint32_t, TimerCallback> _timerCallbacks;
        std::atomic<uint32_t>                       _nextTimerID {1};
    };
} // namespace MMO
```

## A.5 `Src/World/Script/WorldModule.cpp`（新建）

直接从 git `1d3f1a4a:Src/World/MassiveModule.cpp` 搬 15 个 `Bridge_*` 函数（**去掉 3 个 Log**，它们进了 CommonModule），改动清单：

1. `#include "Common/ECS/MassiveModule.h"` → `#include "World/Script/WorldModule.h"`。
2. `#include "World/AutoGen/ProtoBindIndex.gen.h"`：改成只 include World 专属 Proto 注册（见 A.9）。
3. 全局指针 `g_massiveMod` → `g_worldMod`（类型 `WorldModule*`）。
4. 删除 `Bridge_LogInfo/Warn/Error` 三个函数。
5. 构造函数 `Module("massive")` → `Module("world")`。
6. `BindFunctions()`：删掉三行 Log 的 `addExtern`；`RegisterAllProtoMessageTypes` → `RegisterWorldMessageTypes`（World 专属，见 A.9）。
7. 删除 `REGISTER_DYN_MODULE`/`REGISTER_MODULE`（同 A.3 理由）。
8. 新增 `DrainTimers()` 实现（见下）。

```cpp
// WorldModule.cpp 末尾新增
namespace MMO
{
    void WorldModule::DrainTimers()
    {
        // 清空绑旧 Context 的定时器回调，并从时间轮撤销（避免 swap 后 eval 悬垂 ctx）
        for (auto &[timerID, cb] : _timerCallbacks)
        {
            if (_timingWheel)
            {
                _timingWheel->Cancel(timerID);
            }
        }
        _timerCallbacks.clear();
    }
} // namespace MMO
```

> 其余 15 函数体（`ResolveEntity`/`Bridge_EntityPosition`/…/`Bridge_ScheduleTimer` 等）**原样照搬**，只把 `g_massiveMod` 改成 `g_worldMod`、`MassiveModule` 改成 `WorldModule`。`Bridge_ScheduleTimer` 里的 `das_invoke<void>::invoke(ctx.get(), nullptr, block, timerID)` 不变。

## A.6 `Src/World/Script/WorldModuleProvider.h` / `.cpp`（新建）

```cpp
// WorldModuleProvider.h
#pragma once

#include "ScriptEngine/IScriptModuleProvider.h"
#include "World/Script/WorldModule.h"

#include <memory>

namespace MMO
{
    class WorldServer;
    class SceneManager;
    class TimingWheel;
    struct WorldSession;

    class WorldModuleProvider : public IScriptModuleProvider
    {
    public:
        WorldModuleProvider(WorldServer                              *server,
                            SceneManager                             *sceneMgr,
                            TimingWheel                              *timingWheel,
                            std::unordered_map<uint32, WorldSession> *sessions);

        void CreateModules(das::ModuleGroup &group) override;
        void OnContextSwapped(std::shared_ptr<das::Context> ctx) override;
        void OnPreTick(float dt) override;
        void DrainTimers() override;
        const char *EntryScript() const override { return "World/ServerTick.das"; }
        const char *ServiceName() const override { return "world"; }

        WorldModule *World() const { return _world.get(); }

    private:
        std::unique_ptr<WorldModule> _world;
    };
} // namespace MMO
```

```cpp
// WorldModuleProvider.cpp
#include "World/Script/WorldModuleProvider.h"
#include "ScriptEngine/DasEngine.h"

namespace MMO
{
    WorldModuleProvider::WorldModuleProvider(WorldServer *server, SceneManager *sceneMgr,
                                             TimingWheel *timingWheel,
                                             std::unordered_map<uint32, WorldSession> *sessions)
        : _world(std::make_unique<WorldModule>(server, sceneMgr, timingWheel, sessions))
    {
        _world->BindFunctions();
    }

    void WorldModuleProvider::CreateModules(das::ModuleGroup &group)
    {
        // 注：CommonModule 引擎构造时即注册进环境，require Common 会自动解析并加进 libGroup，
        //     故下一行其实可省（无害）。provider 只需加服务专用模块。
        // group.addModule(DasLangEngine::GetIns().Common());
        group.addModule(_world.get());                     // 再加 World 专用模块
    }

    void WorldModuleProvider::OnContextSwapped(std::shared_ptr<das::Context> ctx)
    {
        _world->SetContext(ctx);
    }

    void WorldModuleProvider::OnPreTick(float dt)
    {
        _world->SetScriptDt(dt);
    }

    void WorldModuleProvider::DrainTimers()
    {
        _world->DrainTimers();
    }
} // namespace MMO
```

## A.7 `Src/ScriptEngine/DasEngineConfig.h`（扩展）

```cpp
#pragma once

#include <string>

namespace MMO
{
    enum class EScriptMode
    {
        Develop, // 源码编译 + 热重载 + debugger，不 AOT
        Release, // AOT 优先 + .dabin 缓存/补丁
    };

    struct DasLangEngineConfig
    {
        std::string  dasLangRoot   = "Script"; // daslib/脚本根
        std::string  cacheDir      = "";        // .dabin 缓存目录（空=不缓存）
        std::string  patchDir      = "";        // 热补丁投放目录
        EScriptMode  mode          = EScriptMode::Develop;
        bool         enableWatcher = true;      // 开发期文件监视
        int64        watchPollMs   = 500;
    };
} // namespace MMO
```

## A.8 `DasLangEngine`（重写为服务无关宿主）

见 `ScriptLayerImplementation.md §2.2` 的类声明。此处给关键方法的完整实现骨架（§B/§C/§D 会填充 Compile/HotReload）。阶段 0 只需打通全量编译路径：

```cpp
// DasEngine.cpp（阶段 0 版本，暂不含 .dabin/watcher，占位留接口）
#include "ScriptEngine/DasEngine.h"
#include "ScriptEngine/Module/CommonModule.h"
#include "ScriptEngine/IScriptModuleProvider.h"
#include "Common/Log/Log.h"

#include "daScript/daScriptModule.h"
#include "daScript/simulate/fs_file_info.h"

using namespace das;

namespace MMO
{
    DasLangEngine &DasLangEngine::GetIns()
    {
        static DasLangEngine ins;
        return ins;
    }

    bool DasLangEngine::Initialize(const DasLangEngineConfig &cfg, IDasLangtHost *host,
                                   IScriptModuleProvider *provider)
    {
        if (_initialized)
        {
            return true;
        }
        _cfg      = cfg;
        _host     = host;
        _provider = provider;

        das::setDasRoot(cfg.dasLangRoot);

        // ★★ 必须先注册 daslib 内建模块（含 '$' builtin），否则 CommonModule::BindFunctions
        //    里的 lib.addBuiltInModule() → Module::require("$") 会 DAS_ASSERTF 崩溃
        //    （ast_module.cpp:936 "builtin module not found? or NEED_MODULE(Module_BuiltIn)"）。
        //    顺序铁律：NEED 宏 → 构造模块+BindFunctions(addExtern) → Module::Initialize()。
        NEED_ALL_DEFAULT_MODULES;

        // 引擎持有公共模块（构造即注册进环境；require Common 靠环境全局表解析，
        // 无需 addModule，编译时 ast_parse.cpp:504 会自动把它加进 libGroup）
        _common = std::make_unique<CommonModule>();
        _common->BindFunctions();

        // finalize：走完所有已注册模块的 initDependencies + GC 收尾
        das::Module::Initialize();

        _initialized = true;
        return true;
    }

    das::CodeOfPolicies DasLangEngine::MakePolicies() const
    {
        das::CodeOfPolicies p;
        p.threadlock_context = true; // 持 shared_ptr<Context> 跨阶段
        p.persistent_heap    = true;
        p.rtti               = true;
        if (_cfg.mode == EScriptMode::Develop)
        {
            p.debugger = true;
        }
        else // Release
        {
            p.aot            = true;
            p.fail_on_no_aot = false; // 缺 AOT 回退解释器
        }
        return p;
    }

    void DasLangEngine::BuildModuleGroup(ScriptImage &img)
    {
        img.moduleGroup = std::make_unique<das::ModuleGroup>();
        _provider->CreateModules(*img.moduleGroup); // addModule(Common + 服务专用)
    }

    ScriptImage DasLangEngine::Compile(const std::string &entryFile, const std::string &dabinPath)
    {
        ScriptImage img;
        BuildModuleGroup(img);

        img.access = das::make_smart<das::FsFileAccess>();
        static_cast<das::FsFileAccess *>(img.access.get())->introduceDaslib();

        das::TextWriter logs;
        // 阶段 0：只走全量编译。§B 会在此前插入 .dabin Load。
        img.program = das::compileDaScript(entryFile, img.access, logs, *img.moduleGroup, MakePolicies());
        if (!img.program || img.program->failed())
        {
            img.errors = logs.str();
            Log::Error("Compile failed: {}", img.errors);
            img.program = nullptr;
            return img;
        }
        return img;
    }

    bool DasLangEngine::SimulateImage(ScriptImage &img)
    {
        img.ctx = std::make_shared<das::Context>(img.program->getContextStackSize());
        _provider->OnContextSwapped(img.ctx); // 专用模块重绑 ctx（World 模块 _ctx=）

        das::TextWriter logs;
        if (!img.program->simulate(*img.ctx, logs)) // AOT 在此按 aotHash 链接
        {
            img.errors = logs.str();
            img.ctx    = nullptr;
            return false;
        }
        return true;
    }

    void DasLangEngine::RebindFunctions(ScriptImage &img)
    {
        img.fnUpdate      = img.ctx->findFunction("Update");
        img.fnDispatchMsg = img.ctx->findFunction("dispatch_msg");
        // fnInit 用局部变量，见 Load()
    }

    bool DasLangEngine::Load(const std::string &entryFile)
    {
        _entryFile = entryFile;

        const std::string dabin = /* §B 填缓存路径，阶段0 传 "" */ "";
        ScriptImage img = Compile(entryFile, dabin);
        if (!img.program)
        {
            Log::Error("Load: compile failed: {}", img.errors);
            return false;
        }
        if (!SimulateImage(img))
        {
            Log::Error("Load: simulate failed: {}", img.errors);
            return false;
        }
        RebindFunctions(img);

        if (auto *fnInit = img.ctx->findFunction("Init")) // 一次性，局部
        {
            img.ctx->evalWithCatch(fnInit, nullptr);
            if (auto ex = img.ctx->getException()) Log::Error("Init exception: {}", ex);
        }

        _image = std::move(img);
        _lastGCHeapSize = 0;

        // dispatch 注册（沿用现有 gen.cpp 机制）
        RegisterAllMsgDispatch();
        return true;
    }

    void DasLangEngine::Tick(float dt)
    {
        // §C 会在此插入 swap 检查点
        if (!_image.IsValid()) return;

        _provider->OnPreTick(dt);        // ★ 把 dt 转发给 WorldModule::_scriptDt（不是 OnContextSwapped）
        _image.ctx->restart();

        uint32 sceneID = 1; // 与旧 OnTick 一致（后续可参数化）
        vec4f  args[2] = {das::cast<uint32_t>::from(sceneID), das::cast<float>::from(dt)};

        if (_image.fnUpdate)
        {
            _image.ctx->evalWithCatch(_image.fnUpdate, args);
            if (auto ex = _image.ctx->getException()) Log::Error("Update exception: {}", ex);
        }

        // GC：堆增长超阈值触发（原 WorldServer::OnTick 的逻辑移入引擎）
        uint64 heapNow = _image.ctx->heap->getTotalBytesAllocated();
        if (heapNow - _lastGCHeapSize > 4 * 1024 * 1024)
        {
            _image.ctx->collectHeap(nullptr, true, true);
            _lastGCHeapSize = _image.ctx->heap->getTotalBytesAllocated();
        }
    }

    bool DasLangEngine::DispatchMsg(uint32 sessionID, uint32 msgID, const uint8 *body, size_t len)
    {
        if (!_image.fnDispatchMsg || !_image.ctx) return false;
        // 由 gen.cpp 的 Dispatch 走 GetScriptContext()/GetDispatchFunc()，这里是兜底直调
        // （通常无需——ScriptDispatchRegistry 已覆盖）
        return false;
    }

    void DasLangEngine::Shutdown()
    {
        if (!_initialized) return;
        if (_provider) _provider->DrainTimers();
        _image = ScriptImage{}; // 析构 ctx/program/moduleGroup
        _common.reset();
        das::Module::Shutdown();
        _initialized = false;
    }
} // namespace MMO
```

> **★ dt 同步**：旧代码在 OnTick 里 `_massiveModule->_scriptDt.store(dt)`。分层后 `_scriptDt` 在 WorldModule。给 `IScriptModuleProvider` 加一个 `virtual void OnPreTick(float dt) {}`，`WorldModuleProvider` 转发 `_world->SetScriptDt(dt)`，`Tick` 里调 `_provider->OnPreTick(dt)`。比在引擎里硬编码 WorldModule 干净。（上面代码的 OnContextSwapped 误用已在此说明，实现时用 OnPreTick。）

## A.9 拆分 Proto 注册（`GenMsgBindings.py` + gen.cpp）

现状：`RegisterAllProtoMessageTypes(mod, lib)` 把 Common+Login+Move 全灌进**同一个** module。分层后要拆成两个入口：

- `RegisterCommonMessageTypes(Module&, ModuleLibrary&)` —— 只注册跨服务消息（Common/Login），由 `CommonModule::BindFunctions` 调。
- `RegisterWorldMessageTypes(Module&, ModuleLibrary&)` —— 注册 World 专属消息（Move 等），由 `WorldModule::BindFunctions` 调。

改 `Tools/Script/GenMsgBindings.py`：按消息归属（配置或命名约定）生成两个聚合函数，替代单一 `RegisterAllProtoMessageTypes`。**dispatch 侧同理**：`RegisterAllMsgDispatch` 拆成 `RegisterCommonMsgDispatch` / `RegisterWorldMsgDispatch`。

> 若首版想省事：可以让 `RegisterCommonMessageTypes` 暂时注册全部消息（Common+World 都进 Common 模块），先跑通，Proto 拆分留作后续。但注意这会让 Social 也看到 World 消息定义——与你"专用进各自模块"的决策不符，仅作过渡。

## A.10 `GetDispatchMsgFunction` 命名统一

gen.cpp 调 `server.GetDispatchMsgFunction()`，接口只有 `GetDispatchFunc()`。二选一：

- **推荐**：改 `GenMsgBindings.py` 模板，把 `GetDispatchMsgFunction` → `GetDispatchFunc`（少改一处接口）。
- 或给 `IDasLangtHost` 加 `GetDispatchMsgFunction` 别名。

## A.11 WorldServer 改造（删脚本模拟）

`WorldServer.h`：
```cpp
// 删除 struct DasLangHost _dasHost;（:197-204）整块
// 删除被注释的 _massiveModule（:206）
// 删除 _lastGCHeapSize（:209）—— GC 逻辑连同该基线成员一起移交 DasLangEngine（见 A.8 Tick + 成员）
// 删除声明：InitScriptEngine()（:116）、CompileDaScript(...)（:123）
// 保留：GetScriptContext/GetDispatchFunc/SendRawToClient override、OnTick、OnMessage
// 新增成员：
std::unique_ptr<WorldModuleProvider> _scriptProvider;
```

> `DasLangEngine` 需相应新增私有成员 `uint64 _lastGCHeapSize = 0;`（§A.8 `Tick` 的 GC 用）。

`WorldServer.cpp`：
```cpp
// 三个 getter 真正实现（替换 :202-212 空体）
das::Context *WorldServer::GetScriptContext() const
{
    return DasLangEngine::GetIns().GetScriptContext();
}
das::SimFunction *WorldServer::GetDispatchFunc() const
{
    return DasLangEngine::GetIns().GetDispatchFunc();
}
void WorldServer::SendRawToClient(uint32 sessionID, uint32 msgID, const uint8 *data, size_t len)
{
    // 原 MassiveModule Bridge_SendToClient 就是调这个——把真实发包逻辑填进来
    //（照搬你现有 gate 转发/加密发送路径）
}

// Init 里（原 InitScriptEngine 位置）：
_scriptProvider = std::make_unique<WorldModuleProvider>(this, &_sceneMgr,
                                                        &_logicThread.GetTimingWheel(), &_sessions);
DasLangEngineConfig cfg;
cfg.mode = /* Develop 或 Release */;
cfg.cacheDir = "Bin/.../ScriptCache/world";
DasLangEngine::GetIns().Initialize(cfg, this, _scriptProvider.get());
DasLangEngine::GetIns().Load(_scriptProvider->EntryScript());

// OnTick 里（替换脚本部分）：
void WorldServer::OnTick(std::chrono::milliseconds elapsed)
{
    ProcessUnroutedMessages();
    ProcessControlMessages();
    // ... UpdateLoadLevel ...
    float dt = (float)elapsed.count() / 1000.0f;
    DasLangEngine::GetIns().Tick(dt);      // 内含 swap 检查 + Update
    // CPPSystems / Replicate 照旧
    auto *scene = _sceneMgr.GetDefaultScene();
    if (scene) { /* RunCPPSystems + SystemReplicate */ }
}

// OnMessage 里 ScriptDispatchRegistry::Dispatch 那段不变（getter 现在返回真值了）
// Stop 里删除 _massiveModule/_scriptCtx/_scriptProgram.reset()；改为：
DasLangEngine::GetIns().Shutdown();
```

> **GC 逻辑归属**：旧 OnTick 里的自适应 GC（`heap->getTotalBytesAllocated` 增长触发 `collectHeap`）应移进 `DasLangEngine::Tick`（引擎持有 ctx，逻辑内聚）。`collectHeap(nullptr, true, true)` 首参是 `LineInfo*`（传 nullptr OK）。

**阶段 0 验收**：`xmake` 编译通过；World 进程加载运行 `World/ServerTick.das`（脚本 `require common` + `require world`），Update/dispatch 正常。

---

# §B 阶段 1 — `.dabin` 序列化往返

## B.1 `Src/ScriptEngine/ScriptImage.h`（新建）

见 `ScriptLayerImplementation.md §2.1`（已去掉 `fnInit` 成员）。原样落地即可。

## B.2 `Src/ScriptEngine/ScriptSerializer.h`（新建）

见 `ScriptLayerImplementation.md §2.3` 头文件。

## B.3 `Src/ScriptEngine/DasSerializer.cpp`（补全，★ 复用 `ByteBuffer`）

> **★ 复用基础模块**：不要自己手写字节拼装 / `std::fstream`。用项目现成的 `Common/Core/ByteBuffer.h`——它有 Own/Wrap 双模式、`WriteUint32/ReadUint32/WriteBytes/ReadBytes`、**大端网络序**（跨机器一致，正好适合 `.dabin`）、`MASSIVE_ASSERT` 越界保护。文件读写用 `std::filesystem` + 一次性读入 `ByteBuffer`。

```cpp
#include "ScriptEngine/DasSerializer.h"
#include "Common/Core/ByteBuffer.h"
#include "Common/Log/Log.h"

#include "daScript/ast/ast_serializer.h"
#include "daScript/misc/smart_ptr.h"

#include <filesystem>
#include <fstream>

namespace MMO
{
    // ── 文件读写（一次性，用标准库；无需自定义 stat）──
    static bool ReadWholeFile(const std::string &path, std::vector<uint8> &out)
    {
        std::error_code ec;
        auto sz = std::filesystem::file_size(path, ec);
        if (ec) return false;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        out.resize((size_t)sz);
        return sz == 0 || (bool)f.read((char *)out.data(), (std::streamsize)sz);
    }
    static bool WriteWholeFile(const std::string &path, const uint8 *data, size_t n)
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write((const char *)data, (std::streamsize)n);
        return (bool)f;
    }

    bool DasLangSerializer::Save(const std::string &outPath, das::ProgramPtr program,
                                 das::ModuleGroup &libGroup,
                                 const std::vector<std::pair<std::string, int64>> &deps)
    {
        // 1. 序列化 Program（写模式）—— ★ serializeProgram + thisModuleGroup，非裸 C API
        das::SerializationStorageVector storage;
        {
            das::AstSerializer ser(&storage, /*isWriting*/ true);
            ser.thisModuleGroup = &libGroup;
            ser.serializeProgram(program, libGroup);
            // ser.moduleLibrary = nullptr; // 可选，serializeProgram 内部自管，见 §F
        }

        // 2. 用 ByteBuffer 组文件：头 + 依赖表 + blob（全部大端，ByteBuffer 保证）
        ByteBuffer buf = ByteBuffer::Own(storage.buffer.size() + 256);
        buf.WriteBytes(reinterpret_cast<const uint8 *>("DASBIN"), 6);   // magic[6]
        buf.WriteUint32(kDasbinFormatVersion);
        buf.WriteUint32(das::AstSerializer::getVersion());
        buf.WriteUint32(static_cast<uint32>(sizeof(void *)));
        buf.WriteUint32(static_cast<uint32>(deps.size()));
        for (const auto &[path, mtime] : deps)
        {
            buf.WriteUint16(static_cast<uint16>(path.size()));
            buf.WriteBytes(reinterpret_cast<const uint8 *>(path.data()), path.size());
            buf.WriteUint64(static_cast<uint64>(mtime));
        }
        buf.WriteUint64(static_cast<uint64>(storage.buffer.size()));
        buf.WriteBytes(storage.buffer.data(), storage.buffer.size());

        if (!WriteWholeFile(outPath, buf.Data(), buf.Size()))
        {
            Log::Error("DasSerializer::Save write fail: {}", outPath);
            return false;
        }
        return true;
    }

    das::ProgramPtr DasLangSerializer::Load(const std::string &inPath, das::ModuleGroup &libGroup,
                                            das::FileAccess *fAccess, std::string &outErrors)
    {
        std::vector<uint8> bytes;
        if (!ReadWholeFile(inPath, bytes))
        {
            outErrors = "read fail: " + inPath;
            return nullptr;
        }

        // Wrap 模式零 copy 解析（只读）。ByteBuffer 越界会 MASSIVE_ASSERT，
        // 故先手动做长度下限校验，避免恶意/损坏文件触发断言。
        if (bytes.size() < 6 + 4 * 4)
        {
            outErrors = "truncated header";
            return nullptr;
        }
        ByteBuffer buf = ByteBuffer::Wrap(bytes.data(), bytes.size());

        char magic[6];
        buf.ReadBytes(reinterpret_cast<uint8 *>(magic), 6);
        if (std::memcmp(magic, "DASBIN", 6) != 0) { outErrors = "bad magic"; return nullptr; }

        uint32 fmt   = buf.ReadUint32();
        uint32 dasV  = buf.ReadUint32();
        uint32 ptrSz = buf.ReadUint32();
        uint32 depN  = buf.ReadUint32();
        if (fmt != kDasbinFormatVersion)                 { outErrors = "format version"; return nullptr; }
        if (dasV != das::AstSerializer::getVersion())    { outErrors = "das version"; return nullptr; }
        if (ptrSz != sizeof(void *))                     { outErrors = "pointer size"; return nullptr; }

        // 依赖 mtime 校验（★ 逐条剩余长度检查，防越界断言）
        for (uint32 i = 0; i < depN; ++i)
        {
            if (buf.Size() - buf.ReadPos() < 2) { outErrors = "dep trunc"; return nullptr; }
            uint16 plen = buf.ReadUint16();
            if (buf.Size() - buf.ReadPos() < (size_t)plen + 8) { outErrors = "dep trunc"; return nullptr; }
            std::string path(plen, '\0');
            buf.ReadBytes(reinterpret_cast<uint8 *>(path.data()), plen);
            uint64 mtime = buf.ReadUint64();
            if (fAccess && fAccess->getFileMtime(path) != (int64)mtime)
            {
                outErrors = "stale dep: " + path;
                return nullptr;
            }
        }

        if (buf.Size() - buf.ReadPos() < 8) { outErrors = "blob size trunc"; return nullptr; }
        uint64 blobSize = buf.ReadUint64();
        if (buf.Size() - buf.ReadPos() < blobSize) { outErrors = "blob trunc"; return nullptr; }

        // 反序列化（读模式）—— ★ thisModuleGroup 供复用 native 模块
        das::SerializationStorageVector storage;
        storage.buffer.assign(buf.ReadPtr(), buf.ReadPtr() + blobSize);
        das::ProgramPtr program = das::make_smart<das::Program>();
        {
            das::AstSerializer deser(&storage, /*isWriting*/ false);
            deser.thisModuleGroup = &libGroup;
            deser.serializeProgram(program, libGroup);
        }
        if (program->failed())
        {
            outErrors = "deserialize failed (module hash mismatch / native module missing)";
            return nullptr;
        }
        program->thisModuleGroup = &libGroup;
        return program; // 仍需 simulate
    }
} // namespace MMO
```

> **★ ByteBuffer 越界即断言（非返回错误）**：`ByteBuffer::ReadUint32` 等内部 `MASSIVE_ASSERT(_readPos + count <= _writePos)`，越界会**中止进程**（Release 也在）。所以 `.dabin` 可能被外部投放（热补丁），Load 里对每次读**先手动做剩余长度检查**再 Read，把"损坏文件"降级成返回 `nullptr`（回退全量编译），而不是断言崩溃。上面代码已加这些 guard。
>
> **★ `ByteBuffer::ReadPtr()`**：返回当前读游标指针，配合 `ReadPos()`/`Size()` 做剩余长度判断。blob 直接从 `ReadPtr()` 拷进 `storage.buffer`。

## B.4 `Compile` 接入 `.dabin`

把 §A.8 的 `Compile` 里"只走全量编译"改成"先试 dabin → 回退编译 → 写缓存"：

```cpp
ScriptImage DasLangEngine::Compile(const std::string &entryFile, const std::string &dabinPath)
{
    ScriptImage img;
    BuildModuleGroup(img);
    img.access = das::make_smart<das::FsFileAccess>();
    static_cast<das::FsFileAccess *>(img.access.get())->introduceDaslib();

    // 1. 先试 .dabin
    if (!dabinPath.empty())
    {
        std::string err;
        auto program = ScriptSerializer::Load(dabinPath, *img.moduleGroup, img.access.get(), err);
        if (program) { img.program = program; return img; }
        Log::Info("dabin miss ({}), fall back to full compile", err);
        // ★ Load 失败可能已把 img.moduleGroup 的 library 弄脏（program->library.reset 发生在
        //   反序列化端）。为安全，重建 moduleGroup 再全量编译：
        BuildModuleGroup(img);
    }

    // 2. 全量编译
    das::TextWriter logs;
    img.program = das::compileDaScript(entryFile, img.access, logs, *img.moduleGroup, MakePolicies());
    if (!img.program || img.program->failed())
    {
        img.errors = logs.str(); img.program = nullptr; return img;
    }

    // 3. 写缓存（收集依赖 mtime 需要 simulate 后的 getAllFiles，故缓存写入放 Load() simulate 之后）
    return img;
}
```

> **★ 依赖 mtime 收集时机**：`ctx->getAllFiles()` 要在 simulate 之后才有完整依赖。所以 `.dabin` 写缓存放在 `Load()` 里 `SimulateImage` 成功之后：
> ```cpp
> // Load() 内，SimulateImage 成功后：
> if (!_cfg.cacheDir.empty() && /* 本次是全量编译（非 dabin 命中）*/) {
>     std::vector<std::pair<std::string,int64>> deps;
>     for (auto *fi : img.ctx->getAllFiles())
>         deps.emplace_back(fi->name, img.access->getFileMtime(fi->name));
>     ScriptSerializer::Save(cacheDabinPath, img.program, *img.moduleGroup, deps);
> }
> ```

**阶段 1 验收**：冷启动写 `.dabin`；二次启动命中、跳过编译（日志对比耗时）；改 `.das` 后 mtime 失效回退编译；**反序列化后桥接函数（WorldModule 的 CreateEntity 等）仍可调用**（重点验证硬约束 1）。

---

# §C 阶段 2 — 热重载 / 热补丁

## C.1 `ScriptWatcher.h` / `.cpp`

`.h` 见 `ScriptLayerImplementation.md §2.4`。`.cpp` 完整实现：

> **★ 命名**：与你本地一致用 `DasScriptWatcher`（文件 `DasScriptWatcher.h/.cpp`）。下面示例类名沿用 `ScriptWatcher`，落地时替换即可。

```cpp
#include "ScriptEngine/DasScriptWatcher.h"

#include <filesystem> // ★ 复用标准库，跨平台，不用裸 ::stat

namespace MMO
{
    // 用 std::filesystem 取 mtime+size（跨平台；文件不存在返回 false）
    static bool StatFile(const std::string &path, int64 &mtime, int64 &size)
    {
        std::error_code ec;
        auto st = std::filesystem::last_write_time(path, ec);
        if (ec) return false;
        auto sz = std::filesystem::file_size(path, ec);
        if (ec) return false;
        mtime = (int64)st.time_since_epoch().count();
        size  = (int64)sz;
        return true;
    }

    void ScriptWatcher::Start(std::vector<std::string> files, int64 pollMs, OnChanged cb)
    {
        _pollMs = pollMs; _cb = std::move(cb);
        SetFiles(std::move(files));
        _running.store(true);
        _thread = std::thread([this] { Loop(); });
    }

    void ScriptWatcher::SetFiles(std::vector<std::string> files)
    {
        std::lock_guard lk(_mtx);
        _snapshot.clear();
        for (auto &f : files)
        {
            int64 m = 0, s = 0;
            if (StatFile(f, m, s)) _snapshot[f] = {m, s};
        }
    }

    void ScriptWatcher::Stop()
    {
        _running.store(false);
        if (_thread.joinable()) _thread.join();
    }

    void ScriptWatcher::Loop()
    {
        while (_running.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(_pollMs));
            bool changed = false;
            {
                std::lock_guard lk(_mtx);
                for (auto &[path, snap] : _snapshot)
                {
                    int64 m = 0, s = 0;
                    if (StatFile(path, m, s) && (m != snap.mtime || s != snap.size)) // ★ mtime+size 双判
                    {
                        snap.mtime = m; snap.size = s; changed = true;
                    }
                }
            }
            if (changed && _cb) _cb(); // 只置标志，不做 swap
        }
    }
} // namespace MMO
```

## C.2 请求重载 + Tick swap 检查点 + HotReload

见 `ScriptLayerImplementation.md §4.2/§4.3/§4.4` 的完整代码。要点复述：

- `RequestReload(dabin)` / `RequestReloadFromSource()`：跨线程，`_reloadPending`(atomic) + `_pendingDabin`(mutex)。
- `Tick` 开头：`if (_reloadPending.exchange(false)) HotReload(dabin);` —— **必须在任何 eval 之前**（`restart` 有 `insideContext==0` 断言）。
- `HotReload`：旧 ctx `before_reload`+shutdown → 编新镜像（`_provider->CreateModules` 复用 live 模块）→ 失败保留旧 ctx（安全网）→ `_provider->DrainTimers()` → `_image = std::move(neo)` → `_provider->OnContextSwapped(_image.ctx)` → `after_reload` + `Init`。

## C.3 `Tools/DasPack`

见 `ScriptLayerImplementation.md §5`。**关键**：复用 `WorldModuleProvider::CreateModules` 与共享的 `MakePolicies`。把 provider 构造从 WorldServer 依赖里解耦（DasPack 传桩 host / 空 SceneManager），或给 provider 一个"仅编译用"的轻构造。

**阶段 2 验收**：开发期改源码不重启即生效；`DasPack --service world` 生成 `.dabin` → `RequestReload` → swap 生效；改出语法错时旧脚本继续跑；World 补丁投给 Social 进程 → Load 失败 → 保留旧脚本。

---

# §D 阶段 3 — AOT 基线

见 `ScriptLayerImplementation.md §5（AOT 相关）` 与 `ScriptLayerDesign.md §5`。要点：

1. 新增 `daslang` 可执行 target（源 `ThirdParty/daScript/utils/daScript/main.cpp` + 链 `libDaScript`）。
2. `Src/World/xmake.lua` 加 `rule("das_aot")`（`before_build` + `depend.on_changed`，见 Design §5.3），对 `Script/World/*.das` + `Script/Common/*.das` 生成 `.das.gen.cpp`，`target:add("files", ...)`。
3. Release `MakePolicies` 已开 `aot=true; fail_on_no_aot=false`（§A.8 已写）。
4. `CommonModule`/`WorldModule` 若被 AOT 代码引用其 native 函数，需实现 `Module::aotRequire()` 发 `#include`。

**阶段 3 验收**：release 构建 `findFunction("Update")->aot==true`；打 `.dabin` 补丁后改过的函数 `aot==false`（走解释器）、未改的 `aot==true`。

---

# §E 阶段 0 逐项检查清单（对照敲）

- [ ] A.1 `IScriptModuleProvider.h`
- [ ] A.2/A.3 `CommonModule.h/.cpp`（含删 REGISTER 宏）
- [ ] A.4/A.5 `WorldModule.h/.cpp`（搬 15 函数，改 g_worldMod，加 DrainTimers）
- [ ] A.6 `WorldModuleProvider.h/.cpp`
- [ ] A.7 `DasEngineConfig.h`（EScriptMode）
- [ ] A.8 `DasEngine.h/.cpp`（Initialize/Compile/SimulateImage/RebindFunctions/Load/Tick/Shutdown + OnPreTick 转发）
- [ ] A.9 `GenMsgBindings.py` 拆 Common/World Proto 注册（或过渡：全进 Common）
- [ ] A.10 统一 `GetDispatchFunc` 命名
- [ ] A.11 WorldServer 删脚本成员 + 实现 getter + Init/OnTick/Stop 改造
- [ ] `Src/ScriptEngine/xmake.lua`：`add_deps` 不变（`libDaScript`）；World 的 xmake 增加对 `ScriptEngine` 依赖已有
- [ ] `Src/World/xmake.lua`：把 `Src/World/Script/*.cpp` 纳入编译（`add_files("**.cpp")` 已通配则自动）
- [ ] **编译通过** + World 进程跑通 `World/ServerTick.das`

---

# §G 基础模块复用（项目现成件，别重造轮子）

ScriptEngine 应尽量复用 `Src/Common` 下的基础设施，而不是手写。审查你本地代码时发现有几处自造，以下是对应关系：

| 需求 | ★ 复用 | 位置 | 说明 |
|---|---|---|---|
| `.dabin` 字节读写 | **`ByteBuffer`** | `Common/Core/ByteBuffer.h` | Own/Wrap 双模式；`WriteUint8/16/32/64/Float/Bytes` + `Read*`；**大端网络序**（跨机一致）；`ReadPtr/ReadPos/Size`；越界 `MASSIVE_ASSERT`。§B.3 已改用它 |
| 文件读写 / mtime | **`std::filesystem`** | 标准库 | `file_size`/`last_write_time`/`create_directories`；跨平台，替代裸 `::stat`/`fstream` 手搓。§B.3、§C.1 已改 |
| 引擎/桥接日志 | **`Log`** | `Common/Log/Log.h` | 桥接函数 `Log::At(ELogLevel, file, line, "{}", text)`；引擎内 `Log::Info/Warn/Error`。你已正确复用 ✓ |
| 关键不变量断言 | **`MASSIVE_ASSERT`** | `Common/Core/MassiveAssert.h` | Release 保留；用于"绝不该发生"的前置条件 |
| 基础整型 | **`Types.h`** | `Common/Core/Types.h` | `uint32/int64/...`；`kTickInterval`（tick 周期以此为准，勿硬编码） |
| 配置（模式/目录/轮询间隔） | **`ConfigLoader`** | `Common/Config/ConfigLoader.h` | TOML；`GetString/GetBool/GetUInt32`。`EScriptMode`/`dasbinDir`/`watchPollMs` 从 toml 读，别硬编码 |
| （可选）`.dabin` 加密+签名 | **`Aes256Gcm` + `HmacSha256` + `Hex`** | `Common/Crypto/` | 防逆向层：Save 后加密+签名，Load 先验签+解密到内存再喂 `AstSerializer`。见 §7（设计文档） |
| Proto/网络字节序 | 与 `ByteBuffer` 一致（大端） | — | `.dabin` 依赖表/头都走 ByteBuffer，天然与项目网络协议同序 |

**要点**：`.dabin` 用 `ByteBuffer` 后，与项目现有网络/DB 序列化**同一套字节序和 API**，维护心智一致；越界保护也免费获得（但见 §F 关于外部投放文件要先做长度 guard 的说明）。

---

# §F 易错点总表

| 点 | 说明 |
|---|---|
| `serializeProgram` vs `Program::serialize` | 必须前者 + `thisModuleGroup=&libGroup`，否则 native 函数指针失效 |
| REGISTER_DYN_MODULE/REGISTER_MODULE | C++ 直接 new 模块路线下删除；否则与 NEED_MODULE 动态发现耦合 |
| `collectHeap(nullptr, true, true)` | 首参 `LineInfo*`，nullptr 合法 |
| `Context::restart()` | 有 `insideContext==0` 断言 → swap 只能在 LogicThread、eval 之前 |
| `getAllFiles()` | simulate 后才完整 → 依赖 mtime 收集/写缓存放 simulate 之后 |
| dt 同步 | 分层后 `_scriptDt` 在 WorldModule，用 `provider->OnPreTick(dt)` 转发 |
| `.dabin` Load 失败后 | moduleGroup 的 library 可能被 `program->library.reset()` 弄脏 → 回退编译前重建 moduleGroup |
| DasPack 一致性 | `--service`、policies、daslang 版本、模块集与运行时逐字节对齐，否则 cumulativeHash 不符 |
| `getFileMtime` 未查 stat 返回码 | 文件不存在会返回未初始化 mtime → ScriptWatcher 用 `std::filesystem` 已判存在 |
| **ByteBuffer 越界即断言** | `Read*` 内部 `MASSIVE_ASSERT` 越界会中止进程（Release 也在）。`.dabin` 可能被外部投放（热补丁），Load 每次读**先手动查剩余长度**再 Read，把损坏文件降级成返回 nullptr（§B.3 已加 guard） |
| **ScriptEngine 不 include World** | 引擎公共 API 不能出现 `World/` 符号；dispatch 注册由上层在 Load 后自己调（§0.1） |
| fnInit 不缓存 | 一次性调用用局部变量；只缓存 fnUpdate/fnDispatchMsg |
| ID 大写 | CodingStandard §1.2：sessionID 非 sessionId；命名/gen 模板都遵守 |
| `ser.moduleLibrary = nullptr` | §B.3 写读两端末尾那句是**可选**的（照搬官方 `daScriptC.cpp:666` 惯例，无害）；`serializeProgram` 内部自管此字段，删掉也不影响正确性 |
| `IScriptModuleProvider` 五方法齐全 | `CreateModules`/`OnContextSwapped`/`OnPreTick`/`DrainTimers`/`EntryScript`/`ServiceName` —— A.1 接口、A.6 provider override、A.8 引擎调用三处必须一致（已核对一致） |

---

*本文档所有 API、旧 MassiveModule 结构、WorldServer/gen.cpp 现状均已核实。`serializeProgram + thisModuleGroup + libGroup 复用 native 模块`（§B.3、§F）是 `.dabin` 方案能否正确工作的关键。*
