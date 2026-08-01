# Massive 脚本层设计与执行文档 — DasLang 热重载 / 二进制缓存 / AOT

> 目标：开发期能**快速热重载**脚本；发布期能把脚本**序列化成二进制缓存**减少编译消耗，并支持 **AOT** 获得接近原生的性能。
>
> 本文所有 daScript API / 行为均取自内嵌源码 `ThirdParty/daScript`（版本 `AstSerializer::getVersion()==93`），已逐一在头文件/实现中核实。

---

## 0. 结论速览（TL;DR）

三种能力是**正交**的，对应三个不同问题，可以叠加：

| 能力 | 解决的问题 | 机制 | 目标构建 |
|---|---|---|---|
| **热重载** | 开发期改脚本要重启服务器 | 文件监视 → 重新 `compileDaScript`+`simulate` → tick 之间原子 swap Context | debug / releasedbg |
| **二进制缓存** | 启动时全量编译（parse+infer+optimize）慢 | `AstSerializer` 把编译好的 `Program`(AST) 序列化到磁盘，启动时反序列化跳过编译（仍需 `simulate`） | 所有构建（尤其发布） |
| **AOT** | 解释执行慢 | 构建期把 `.das` transpile 成 `.cpp` 编进宿主，运行时按语义哈希把解释节点替换为原生 C++ 调用 | release |

**推荐落地顺序**（每阶段独立可用、可验证）：

1. **阶段 0（前置，必做）**：修复当前无法编译的半完成重构，统一脚本引擎为单一路径。
2. **阶段 1**：`DasLangEngine` 引入 `CodeOfPolicies`，抽象出统一的"编译 + simulate + 缓存函数指针"入口。
3. **阶段 2**：开发期热重载（文件监视 + LogicThread 内 swap）。
4. **阶段 3**：二进制缓存（序列化 Program，mtime 判过期）。
5. **阶段 4**：AOT（先构建 `daslang` 工具，再加 xmake codegen rule，宿主开 `policies.aot`）。

---

## 1. 当前现状（一手核实）

### 1.1 脚本引擎结构
- `Src/ScriptEngine/`：`DasEngine.{h,cpp}`（`DasLangEngine` 单例）、`DasEngineConfig.h`、`IDasHost.h`、`DasHelpers.h`、`Module/DasCommonModule.{h,cpp}`。
- `DasLangEngine`：`Initialize`（`setDasRoot` + `NEED_ALL_DEFAULT_MODULES` + `NEED_MODULE(DasCommonModule)` + `Module::Initialize`）/ `CompileScript`（`FsFileAccess` + `introduceDaslib` + `compileDaScript`）/ `CreateContext`。
- daScript 完整源码 + `.rst` 文档 + `skills/` 内嵌于 `ThirdParty/daScript`（权威来源，优于 `DasLangDoc/*.html`）。

### 1.2 实际运行路径（`Src/World/WorldServer.cpp`）
WorldServer **绕过** `DasLangEngine`，自己走一套：`InitScriptEngine()`（:665）流程：

```
a. das::Module::Initialize()                                          [:673]
b. _massiveModule = make_unique<MassiveModule>(this, &_sceneMgr, ...) [:677]
c. _massiveModule->BindFunctions()                                    [:679]
d. das::ModuleGroup libGroup; libGroup.addModule(_massiveModule.get())[:682]
e. _scriptProgram = CompileDaScript("Script/ServerTick.das", libGroup)[:685]
     └ 内部: FsFileAccess + introduceDaslib() + das::compileDaScript() [:639]
f. _scriptCtx = make_shared<das::Context>(program->getContextStackSize())[:693]
g. _massiveModule->_ctx = _scriptCtx                                  [:696]
h. _scriptProgram->simulate(*_scriptCtx, logs)                        [:699]
i. findFunction("Init") → evalWithCatch → 缓存 _fnInit               [:706]
j. _fnUpdate = findFunction("Update")                                [:723]
k. _fnDispatchMsg = findFunction("dispatch_msg")                     [:730]
l. RegisterAllMsgDispatch()   (ProtoBindIndex.gen.cpp)               [:734]
```

- **逻辑线程**：单一 `LogicThread`（`LogicThread.cpp:49 RunLoop`，20ms 固定 tick）。脚本 `Update` 在 `WorldServer::OnTick`（:259），由 RunLoop Phase4 `onTick(budget)` 触发。消息分发 `OnMessage`（:419）→ `ScriptDispatchRegistry::Dispatch` → `*.gen.cpp` → `ctx->eval`。
- **所有脚本触碰都在 LogicThread 内**；IO 线程仅经 `_sessionsMtx` 访问 `_sessions`，不碰脚本。

### 1.3 阶段 0 前置：当前代码库无法编译（半完成重构，必须先修）

| # | 阻断项 | 位置 | 处理 |
|---|---|---|---|
| 1 | `#include "Common/ECS/MassiveModule.h"` 悬空（文件已从磁盘删除） | `WorldServer.cpp:8` | 从 git `1d3f1a4a` 恢复，或迁入 `Src/ScriptEngine/Module/` |
| 2 | `WorldServer.h` 把脚本成员包进 `struct DasLangHost _dasHost{}` 并注释掉 `_massiveModule`，而 `.cpp` 全程用扁平成员 | `WorldServer.h:197-206` vs `.cpp` | 统一为扁平成员布局 |
| 3 | `GetScriptContext()` / `GetDispatchFunc()` / `SendRawToClient()` 空函数体（无 return，UB） | `WorldServer.cpp:202-210` | 真正实现，返回对应成员 |
| 4 | 生成代码调 `server.GetDispatchMsgFunction()`，接口只声明 `GetDispatchFunc()` | `Move.gen.cpp:77` / `IDasHost.h` | 统一命名（注意 §1.2 ID 大写）；同步改 `GenMsgBindings.py` 模板 |
| 5 | 两套并行桥接：旧 `MassiveModule`(15 函数，在用) vs 新 `DasCommonModule`(3 个 Log，未接线)；`WorldServer::CompileDaScript` 与 `DasLangEngine::CompileScript` 重复 | 多处 | 收敛为单一路径，AOT/cache 钩子才不分叉 |
| 6 | `DasLangEngineConfig.dasLangRoot="Script"` 与硬编码 `"Script/ServerTick.das"` 不统一 | — | 统一 das root 来源 |

> **阶段 0 是所有后续工作的前提**：不先收敛到单一编译入口，缓存/AOT 钩子会分叉到两条路径上。建议把 WorldServer 的脚本加载全部收敛到 `DasLangEngine`。

---

## 2. 统一脚本引擎入口（阶段 1）

把"编译 → simulate → 缓存函数指针 → 迁移状态"抽象成一个可重用的编译单元，热重载/缓存/AOT 都复用它。

### 2.1 编译单元结构（照搬 daslang-live 的析构顺序约束）

```cpp
// Src/ScriptEngine/DasEngine.h
namespace MMO
{
    // 一次脚本编译的完整产物，作为整体 swap
    struct ScriptImage
    {
        // moduleGroup 必须声明在最前 → 最后析构。
        // 因为 Program/Context 通过 library.modules 持有指入 moduleGroup 的裸 Module*，
        // moduleGroup 先于它们析构会造成悬垂。
        std::unique_ptr<das::ModuleGroup> moduleGroup;
        das::FileAccessPtr                access;
        das::ProgramPtr                   program;
        std::shared_ptr<das::Context>     ctx;
        std::string                       errors;

        bool IsValid() const { return ctx != nullptr; }
    };
} // namespace MMO
```

### 2.2 引入 CodeOfPolicies（AOT / 缓存 / 热重载 都靠它）

`CompileScript` 增加 `CodeOfPolicies` 形参（当前无），贯穿到 `compileDaScript`：

```cpp
// 三个构建档位对应的策略
das::CodeOfPolicies MakePolicies(EScriptMode mode)
{
    das::CodeOfPolicies p;
    p.threadlock_context = true;   // 持 shared_ptr<Context> 跨阶段必需
    p.persistent_heap    = true;   // string builder 免 unsafe
    p.rtti               = true;   // 反射（热重载状态迁移需要）
    switch (mode)
    {
        case EScriptMode::Develop:      // 开发：热重载，不缓存不 AOT
            p.debugger = true;
            break;
        case EScriptMode::Release:      // 发布：AOT 优先
            p.aot             = true;
            p.fail_on_no_aot  = false; // 缺 AOT 静默回退解释器（健壮）
            break;
    }
    return p;
}
```

> **注意反直觉语义**：`fail_on_no_aot=true` = 缺 AOT 即编译失败（用于 CI 抓漏）；`false` = 缺 AOT 静默回退解释器（用于生产健壮发布）。默认是 `true`，**生产要显式设 false**。

---

## 3. 开发期热重载（阶段 2）

**参考实现**：`ThirdParty/daScript/utils/daslang-live/main.cpp`(953 行) + `modules/dasLiveHost/` + `live/live_watch.das`。直接照搬其 swap 骨架，不要自己发明。

### 3.1 变更检测（文件监视）

`FileInfo` 本身不存 mtime，必须自己轮询：

```cpp
// simulate 成功后，取该 Context 引用的全部源文件（含所有 require 依赖）
std::vector<das::FileInfo*> files = image.ctx->getAllFiles();   // simulate.h:768
// 对每个 fi->name 记录快照
for (auto* fi : files)
{
    int64 mtime = image.access->getFileMtime(fi->name);         // debug_info.h; stat().st_mtime
    // 同时记录文件 size —— 捕获"同一秒内的快速编辑"（live_watch.das 的做法）
    _watchSnapshot[fi->name] = { mtime, FileSize(fi->name) };
}
```

- 后台线程每 ~0.5s 重新 `stat`，**同时比对 mtime 和 size**，任一变化 → 置 `_reloadPending` 原子标志。
- 依赖集会随脚本 `require` 变化，每次 swap 后重建监视清单。

### 3.2 安全 swap 点

`Context::restart()` 内部有 `DAS_ASSERTF(insideContext==0)` —— **不能在任何线程正在旧 Context 内执行时 swap**。因为所有脚本执行都在 LogicThread，安全点是：

> **LogicThread 内、`OnTick` 开头、任何 `eval` 之前**，检查 `_reloadPending`。

全程单线程无并发，**无需加锁**。切勿从 IO 线程或文件监视线程触发指针替换。

### 3.3 swap 流程

```cpp
void WorldServer::OnTick(...)
{
    if (_reloadPending.exchange(false))
    {
        DoHotReload();   // 在任何脚本 eval 之前
    }
    // ... 正常 tick: _scriptCtx->restart(); evalWithCatch(_fnUpdate, args); ...
}

void WorldServer::DoHotReload()
{
    // (a) 旧 Context 收尾
    _scriptImage.ctx->restart();
    // [before_reload] 保存状态（见 3.4）
    CallAnnotated(_scriptImage.ctx, "before_reload");
    if (_fnShutdown) _scriptImage.ctx->evalWithCatch(_fnShutdown, nullptr);

    // (b) 编译新的（全新 access + moduleGroup，天然绕开陈旧缓存）
    ScriptImage neo = _engine.Compile("Script/ServerTick.das", MakePolicies(EScriptMode::Develop), _massiveModule.get());
    if (!neo.IsValid())
    {
        Log::Error("HotReload compile failed, 保留旧脚本继续运行: {}", neo.errors);
        // 安全网：编译失败保留旧 Context，恢复运行
        _scriptImage.ctx->restart();
        CallAnnotated(_scriptImage.ctx, "after_reload");
        if (_fnInit) _scriptImage.ctx->evalWithCatch(_fnInit, nullptr);
        return;
    }

    // (c) 原子替换 → 旧 ctx/program/moduleGroup 此刻析构
    _scriptImage = std::move(neo);
    _scriptCtx   = _scriptImage.ctx;
    _massiveModule->_ctx = _scriptCtx;

    // (d) 重新抓取一切缓存的指针/索引（关键！）
    RebindScriptFunctions();     // 见 3.5
    DrainTimerCallbacks();       // MassiveModule::_timerCallbacks 持旧 Context，必须 drain

    // (e) 迁移状态 + 重新初始化
    RebuildWatchList();
    _scriptCtx->restart();
    CallAnnotated(_scriptCtx, "after_reload");   // after_reload 在 init 之前
    if (_fnInit) _scriptCtx->evalWithCatch(_fnInit, nullptr);
    _lastGCHeapSize = 0;
}
```

> 两个 Context 在 (b)~(c) 间短暂并存：旧的已 `shutdown` 但对象仍在，新的编译完成；(c) 的 `std::move` 才真正析构旧的（这是安全网的基础）。

### 3.4 状态迁移

新 Context 全局变量清零（`restart()` 只回卷 stack + string heap，**不回卷 general heap**；换 Context 才真正释放脚本全局）。两条路径：

- **手动**：`[before_reload]` 里序列化全局到持久 byte store，`[after_reload]` 反序列化回来（`after_reload` 在 `init()` 之前跑）。
- **自动**：给全局加 `@live` 宏（`require live/live_vars`），宏生成 `__before_reload_live_vars` / `__after_reload_live_vars`，host 按名字前缀发现并调用。
- **限制**：指针 / lambda / handle 不能自动序列化，需手动 `reinterpret<uint64>` 存取。

> `g_handler_registry`（`Script/AutoGen/HandlerRegistry.das`）由 `[msg_handler]` 宏在 `[init]` 里 `qmacro` 注入、simulate 时运行时填充 —— **新 Context 重新 simulate 会自动重建**，无需手搬；但 `validate_handler_registry()` 要在新 Context 重跑。

### 3.5 swap 后必须重新抓取的全部失效项

| # | 项 | 说明 |
|---|---|---|
| 1 | `_scriptCtx` (shared_ptr\<Context\>) | 新 Context |
| 2 | `_scriptProgram` (ProgramPtr) | 新 Program |
| 3 | `_fnInit` (SimFunction\*) | `findFunction("Init")` 重取 |
| 4 | `_fnUpdate` (SimFunction\*) | OnTick 每帧读，必须重取 |
| 5 | `_fnDispatchMsg` (SimFunction\*) | 经 `GetDispatchFunc` 供所有 gen.cpp |
| 6 | `MassiveModule::_ctx` | 桥接函数分配 TArray 用 |
| 7 | `MassiveModule::_timerCallbacks[].ctx` | 每个持旧 Context 的 shared_ptr，**必须 drain/重建**，否则定时器触发踩悬垂 Context（`Stop()` 已用 `clear()` 处理销毁场景 :180，热重载需同样处理） |
| 8 | `_lastGCHeapSize` | 归零 |
| — | 所有 `findVariable` 返回的 int 索引 / `getVariable(idx)` 裸指针 | swap 后立即失效 |

> `ScriptDispatchRegistry` 表里的 `ScriptDispatchFn` 是 C++ 静态函数指针，本身不需刷新；但它们运行时经 `server.GetScriptContext()` / `GetDispatchFunc()` 取 #1/#5 —— 只要这两个 getter 返回新值即可（前提：阶段 0 先修好空实现）。

### 3.6 关键 API 速查（热重载）

| API | 头 | 用途 |
|---|---|---|
| `FsFileAccess` | `simulate/fs_file_info.h` | `make_smart<FsFileAccess>()`+`introduceDaslib()`；每次重载**新建**绕开缓存 |
| `FileAccess::getFileMtime(name):int64` | `debug_info.h`（impl `module_file_access.cpp:79`） | `stat().st_mtime`；DAS_NO_FILEIO 返 -1 |
| `FileAccess::invalidateFileInfo(name)` / `reset()` | `debug_info.h` | 单条失效 / 清整个缓存（若坚持复用 access） |
| `compileDaScript(fn, access, logs, libGroup, policies)` | `ast.h:1831` | 每次产生全新 Program |
| `SimulateWithErrReport(program, tw):ContextPtr` | `misc/das_common.h:9` (inline) | simulate 成新 Context 的标准封装，失败返 nullptr |
| `Context::getAllFiles():vector<FileInfo*>` | `simulate.h:768` | 完整监视清单（含所有 require 依赖） |
| `Context::findFunction/findVariable/getVariable` | `simulate.h:534/426` | swap 后重取 |
| `Context::restart()` | `simulate.h:447` | 有 `insideContext==0` 断言，界定安全 swap 点 |

---

## 4. 二进制缓存（阶段 3）

把编译好的 `Program`(AST) 序列化到磁盘，启动时反序列化，**跳过 parse + infer + optimize**（省的是编译时间；反序列化后仍需 `simulate` 建 Context，性能仍是解释器）。

### 4.1 流水线定位

```
源码 ──parse+infer+optimize──▶ AST/Program ──simulate──▶ Context ──eval──▶ 运行
                                    ▲                                        
                        序列化/反序列化在这里                                 
                        （缓存 Program，省编译；不省 simulate）               
```

### 4.2 API（最干净路径：C API，`src/misc/daScriptC.cpp:661`）

```cpp
// 写缓存
das_serialized_data* das_program_serialize(das_program*, const void** out_data, int64_t* out_size);
// 读缓存
das_program* das_program_deserialize(const void* data, int64_t size);
void das_serialized_data_release(das_serialized_data*);
```

C++ 侧等价（`ast_serializer.h`）：
```cpp
das::SerializationStorageVector storage;                 // vector<uint8_t> buffer
{ das::AstSerializer ser(&storage, /*writing*/true);  program->serialize(ser); }
// storage.buffer 即缓存字节；反序列化：
{ das::AstSerializer deser(&storage, /*writing*/false); program = make_smart<Program>(); program->serialize(deser); }
```

### 4.3 过期检测（照搬 daScript 内建 mtime 机制）

内建缓存路径 `ast_parse.cpp:530 trySerializeProgramModule` 的做法：

```cpp
int64_t file_mtime  = access->getFileMtime(fileName);
int64_t saved_mtime = <从缓存头读出>;
if (saved_filename != fileName || file_mtime != saved_mtime) {
    // 缓存过期 → 回退全量编译
}
```

**我们的缓存文件格式**（自定义头 + blob）：
```
[magic "MSVC"] [daslang version = 93] [pointer size = 8] [entry .das 路径]
[依赖文件数 N] [N × {路径, mtime}]   ← 覆盖所有 require 依赖（用 getAllFiles / getPrerequisits 枚举）
[serialized Program blob]
```

加载逻辑：
```cpp
ScriptImage LoadFromCache(path, policies) {
    读缓存头;
    if (version != AstSerializer::getVersion()) return {};        // 版本不符
    if (pointerSize != sizeof(void*))            return {};
    for (auto& [dep, savedMtime] : deps)
        if (access->getFileMtime(dep) != savedMtime) return {};   // 任一依赖变了 → 失效
    program = das_program_deserialize(blob, size);
    if (!program || program->failed())            return {};
    // 反序列化成功 → 仍需 simulate
    ctx = SimulateWithErrReport(program, logs);
    return { ..., program, ctx };
}
```

### 4.4 集成点

`InitScriptEngine` 步骤 e 改为：**先试 `LoadFromCache`，失败回退全量 `compileDaScript` 并 `SaveToCache`**。

### 4.5 陷阱（一手确认）

- serialize / load 两端必须：**模块集合一致**、**daslang 版本一致（v93）**、**指针宽度一致**。
- `daslib/debug` 模块会禁用序列化（`disableSerializationOnDebugger`）—— 开发期开 debugger 时缓存自动失效，符合预期。
- `CodeOfPolicies::serialize_main_module=true`（默认）；设 false 则每次重编 main module。
- 缓存文件应放在构建输出目录（如 `Bin/<plat>/ScriptCache/`），随构建清理。

---

## 5. AOT（阶段 4）

构建期把 `.das` transpile 成 `.cpp` 编进宿主；运行时按**语义哈希**把解释节点替换为原生 C++ 调用。**不是 JIT**。

### 5.1 前置条件：项目当前不产出 `daslang` 工具

`ThirdParty/daslang.lua` 只构建三个静态库（`libUriParser` / `libDaScript_runtime` / `libDaScript`），**没有 `daslang` 可执行**。AOT 代码生成器 `utils/aot/main.das` 需要它。

> **必做**：新增一个 `daslang` 可执行 target（编译 `utils/main/*.cpp` 之类的 CLI 入口 + 链接 `libDaScript`），或用官方 CMake 单独 build 一次 `daslang.exe` 当工具用。这是 AOT 的硬前置。

### 5.2 三阶段工作流

**Stage 1 — 生成 C++**（构建期）：
```bash
daslang utils/aot/main.das -- -aot Script/ServerTick.das ServerTick.das.gen.cpp
# 批量: -aot in1 out1 -aot in2 out2 ...；或 -in_file batch.txt
```
生成文件尾部含 `static AotListBase impl(registerAotFunctions);` —— 进程启动时自注册进全局 `AotLibrary`。**宿主无需手写任何 register 调用**。

**Stage 2 — 编译进宿主**：生成的 `.gen.cpp` 与宿主一起编译、链接 `libDaScript`。

**Stage 3 — 运行时链接**：
```cpp
CodeOfPolicies policies;
policies.aot            = true;    // 必需
policies.fail_on_no_aot = false;   // 生产：缺 AOT 回退解释器
auto program = compileDaScript(scriptPath, fAccess, tout, libGroup, policies);
auto ctx = ...; program->simulate(*ctx, tout);   // simulate 内部自动 linkCppAot 按哈希绑定
if (ctx->findFunction("Update")->aot) { /* 跑原生 C++ */ }
```

> **重要**：`Program::linkCppAot` 是 `protected` 且源码注释标注 *"no longer the way to link AOT — set CodeOfPolicies::aot instead"*，**不可手动调用**。唯一支持路径是给 `compileDaScript` 传 `policies.aot=true`，`simulate()` 内部（`aot_hint = policies.aot && !folding && !thisModule->isModule`）自动完成链接。

### 5.3 xmake codegen rule

用 `before_build` + `depend.on_changed`（工具/源改了才重生成），生成的 `.cpp` 用 `target:add("files", ...)` 进编译：

```lua
-- 挂到 ScriptEngine 或 World target
rule("das_aot")
    set_extensions(".das")
    before_build(function (target, sourcebatch, opt)
        import("core.project.depend")
        local dasexe = path.join("$(projectdir)", "Bin/<plat>/daslang")   -- 阶段前置产物
        local aotmain = path.join("$(projectdir)", "ThirdParty/daScript/utils/aot/main.das")
        for _, dasfile in ipairs(sourcebatch.sourcefiles) do
            local outcpp = path.join(target:autogendir(), "aot", path.basename(dasfile) .. ".das.gen.cpp")
            depend.on_changed(function ()
                os.mkdir(path.directory(outcpp))
                os.execv(dasexe, {aotmain, "--", "-aot", dasfile, outcpp})
            end, {files = {dasfile, dasexe}, dependfile = outcpp .. ".d"})
            target:add("files", outcpp)
        end
    end)
```

> **注意**：`Src/World/xmake.lua:9-36` 已有 `rule("gen_msg_bindings")` 用 `on_load` 的先例（注释解释了为何通配符 sourcebatch 固化时机的坑）。若用 `add_files("Script/*.das")` 触发本 rule，遵循同样模式。

### 5.4 语义哈希机制 —— AOT 最大的坑

每函数两级哈希（`src/simulate/simulate_fn_hash.cpp`，64-bit FNV1a + `hash_block64`）：
- `Function::hash`（own）= 整棵 SIM 节点树（节点类型/常量/字符串字面量/字段偏移）+ 结果/参数类型的语义哈希。
- `Function::aotHash`（注册表 key）= `hash_block64([own_hash, 各传递依赖的 own_hash（按 mangled name 稳定排序）])`。

**哈希漂移 = `error[50101] AOT link failed`（或静默回退）。以下任一变化都会漂移**：
- 脚本源码（及**所有传递依赖模块**）改动 → 必须重生成 `.cpp`。
- daslang 版本不同。
- 影响 codegen 的编译选项不同（生成端与运行端必须逐一对齐）。
- 内嵌路径字符串 `/` vs `\` 不一致（进 SIM 树参与哈希）→ 需归一化路径分隔符。
- `aot_enabled()` / `is_in_aot()` 这类 `SideEffects::none` 且依赖 policy 的函数会被常量折叠成不同 AST（生成期 false、运行期 true）→ **断言逻辑不要碰它们**。
- `solid_context=true` 会**禁用** AOT（全索引访问）。

绑定原生函数（`MassiveModule`）的 C++ 模块必须正确实现 `Module::aotRequire()` 发 `#include`，否则生成的 C++ 编译不过。

### 5.5 Standalone 模式（可选，终极发布形态）

`-ctx` / `CodeOfPolicies::standalone_context=true`（`runStandaloneVisitor`）生成 `class Standalone : public Context`，构造函数内联复刻 `simulate` 全流程（setup / InitGlobalVar / FillFunction 按 aotHash 绑定 / runInitScript）。宿主 `new Standalone()` 即得已编译好的 Context，**运行时无需 `compileDaScript` / `simulate`**。
- 限制：入口函数参数 / 返回**不能是结构体**（会 panic）。
- `-aotlib` 模式把 daslib 等库模块预编成 `libDaScriptAot` 静态库桩，供多个 AOT 二进制共享。

### 5.6 关键 API 速查（AOT）

| API | 头 | 用途 |
|---|---|---|
| `CodeOfPolicies::aot / fail_on_no_aot / standalone_context / cross_platform` | `ast.h:1509` | 开关 |
| `getGlobalAotLibrary() : AotLibrary&` | `simulate/aot_library.h` | 宿主唯一入口（simulate 内部用） |
| `AotListBase` | `aot_library.h` | 生成代码自注册链表节点 |
| `SimFunction::aot`（位域） | `simulate.h` | 运行时判断是否走 AOT |
| `das_function_is_aot(fn)` | `daScriptC.h` | C API 查询 |

---

## 6. 分阶段执行清单（可逐步验证）

### 阶段 0 — 修复编译（前置）
- [ ] 恢复 / 迁移 `MassiveModule`（git `1d3f1a4a`），或收敛进 `DasCommonModule`。
- [ ] 统一 `WorldServer.h` / `.cpp` 成员布局（扁平）。
- [ ] 实现 `GetScriptContext()` / `GetDispatchFunc()` / `SendRawToClient()`。
- [ ] 统一 `GetDispatchFunc` vs `GetDispatchMsgFunction` 命名（含 `GenMsgBindings.py`）。
- [ ] 收敛 `WorldServer::CompileDaScript` → `DasLangEngine`，单一 das root。
- [ ] **验证**：`xmake` 编译通过，服务器能加载并运行 `ServerTick.das`。

### 阶段 1 — 统一引擎入口
- [ ] `DasLangEngine::CompileScript` 增加 `CodeOfPolicies` 形参。
- [ ] 引入 `ScriptImage` 结构 + `MakePolicies(mode)`。
- [ ] **验证**：debug/release 两档策略各自编译运行正常。

### 阶段 2 — 热重载（开发期）
- [ ] 文件监视线程（`getAllFiles` + `getFileMtime` + size，0.5s 轮询，置 `_reloadPending`）。
- [ ] `OnTick` 开头 swap（`DoHotReload`）+ 重抓 3.5 表全部失效项 + drain `_timerCallbacks`。
- [ ] 编译失败保留旧 Context 安全网。
- [ ] （可选）状态迁移：`[before_reload]`/`[after_reload]` 或 `@live`。
- [ ] **验证**：运行中改 `ServerTick.das` 的 `Update`，不重启即生效；改出语法错误时服务器继续用旧脚本运行并打错误。

### 阶段 3 — 二进制缓存
- [ ] 缓存文件格式（头 + 依赖 mtime 列表 + blob）+ `SaveToCache` / `LoadFromCache`。
- [ ] `InitScriptEngine`：先试缓存，失败回退编译并写缓存。
- [ ] **验证**：冷启动写缓存；二次启动命中缓存、跳过编译（对比启动耗时日志）；改任一 `.das` 后缓存失效回退全量编译。

### 阶段 4 — AOT
- [ ] 新增 `daslang` 可执行 target（AOT 工具前置）。
- [ ] `rule("das_aot")` + 挂到 target + `add_files("Script/*.das")`。
- [ ] release 档 `policies.aot=true; fail_on_no_aot=false`。
- [ ] `MassiveModule` 实现 `Module::aotRequire()`。
- [ ] **验证**：release 构建，`findFunction("Update")->aot==true`；CI 用 `fail_on_no_aot=true` 抓未 AOT 的函数；改脚本后重新生成 `.gen.cpp` 并重编。

---

## 7. 附：一手 API 纠正备忘

- `Context::collectHeap(LineInfo* at, bool stringHeap, bool validate)` — 首参是 `LineInfo*` 非 `char*`（`WorldServer.cpp:304` 传 `nullptr` 才恰好通过）。
- `Context::restart()` 只回卷 stack + string heap，**不回卷 general heap** —— 热重载必须丢弃旧 Context 才真正释放脚本全局。
- `Program::linkCppAot` 是 `protected` + 已弃用，**不可外部调用**；用 `CodeOfPolicies::aot=true`。
- `LogicThread::RunLoop` 是 `private` 回调式签名（非 vararg），外部不可直接调；脚本触碰全在其回调内 → LogicThread 单线程安全点成立。
- `src/ast/ast_aot_cpp.cpp` 已被清空 —— AOT C++ 发射器实际在 `daslib/aot_cpp.das`（用 daslang 写的 `CppAot` 访问器）。

---

*本文档基于对 `ThirdParty/daScript` 源码的深度核实（4 支柱并行研究 + 对抗验证）。所有 daScript API 均已在头文件/实现中确认存在。*
