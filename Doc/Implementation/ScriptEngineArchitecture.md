# Massive 脚本引擎分层重构方案

> **版本**: v1.0  
> **日期**: 2026-07-29  
> **基于分析**: WorldServer 多实例 + 计划嵌入 SocialServer  
> **源文档参考**: [分层实现方案（你提供的原始方案）]、[23_ProtoScriptBinding.md]、[CodingStandard.md]

---

## 一、总体目标

1. **消除代码重复**：`WorldServer::InitScriptEngine()` 中 68 行初始化逻辑将在 SocialServer 中重复，提取为 `DasEngine`。
2. **解耦生成代码与 WorldServer**：当前 `*.gen.cpp` 中 `DispatchXxxReq(WorldServer&, ...)` 硬编码了服务器类型，引入 `IScriptHost` 接口使其可复用于任意服务器。
3. **统一绑定注册入口**：`RegisterDasBindings()` + `RegisterAllProtoMessageTypes()` 分散在多处，统一为 `BindingRegistry::RegisterAll()`。
4. **保持现有架构不变**：单 LogicThread、单 Context、无热重载、无 AOT。

---

## 二、新增/修改文件清单

### 2.1 新增文件

| 文件 | 说明 | 行数估计 |
|------|------|----------|
| `Src/Engine/DasEngine.h` | 脚本引擎封装——Init/Shutdown/CompileScript/CreateContext | ~60 |
| `Src/Engine/DasEngine.cpp` | 实现 | ~100 |
| `Src/Engine/DasEngineConfig.h` | 配置结构体 | ~40 |
| `Src/Engine/IScriptHost.h` | 服务器宿主接口——Context/Dispatch/SendRaw | ~50 |
| `Src/Engine/Binding/BindingRegistry.h` | 绑定注册统一入口 | ~30 |
| `Src/Engine/Binding/BindingRegistry.cpp` | 实现 | ~40 |
| `Src/Engine/Binding/DasCoreBindings.cpp` | 迁移自 `Src/Engine/DasBindings.cpp` | ~110（迁移） |
| `Src/Engine/xmake.lua` | Engine 静态库构建定义 | ~30 |

### 2.2 修改文件

| 文件 | 修改内容 | 影响行数 |
|------|----------|----------|
| `Src/World/WorldServer.h` | 继承 `IScriptHost`，移除私有脚本成员到 `_scriptHost` | ~20 |
| `Src/World/WorldServer.cpp` | `InitScriptEngine()` 改为调用 `DasEngine`；`OnTick()` 微调 | ~30 |
| `Src/World/MassiveModule.cpp` | `BindFunctions()` 改为调用 `BindingRegistry::RegisterAll()` | ~5 |
| `Src/World/ScriptDispatchRegistry.h` | `Dispatch()` 参数从 `WorldServer&` 改为 `IScriptHost&` | ~3 |
| `Src/World/ScriptDispatchRegistry.cpp` | 同上 | ~3 |
| `Src/World/AutoGen/*.gen.cpp` | 重新生成——`WorldServer&` → `IScriptHost&` | ~0（重新生成） |
| `Tools/Script/GenMsgBindings.py` | 生成模板中 `WorldServer&` → `IScriptHost&`，include 路径微调 | ~10 |
| `Src/World/xmake.lua` | 新增对 `Engine` 库的依赖 | ~1 |
| `Src/Engine/DasBindings.cpp` | 删掉，内容迁移到 `DasCoreBindings.cpp` | 文件删除 |

### 2.3 不变文件

| 文件 | 不变原因 |
|------|----------|
| `Src/Engine/DasHelpers.h` | 独立工具，不受影响 |
| `Src/Common/ECS/MassiveModule.h` | 保持现有接口 |
| `Script/*.das` (全部) | 纯脚本层，不受 C++ 重构影响 |
| `Script/AutoGen/HandlerRegistry.das` | 由 GenMsgBindings.py 重新生成，内容不变 |
| `Config/*.toml` | 配置格式不变 |
| `ThirdParty/*` | 第三方库不变 |

---

## 三、详细设计

### 3.1 DasEngineConfig — 配置结构体

```cpp
// Src/Engine/DasEngineConfig.h
#pragma once
#include <string>

namespace MMO
{
    struct DasEngineConfig
    {
        std::string dasRoot = "Script";    // daScript 生态根目录（daslib/所在）
        bool        enableDebugger = false; // 未来扩展：daScript debugger
    };
}
```

**设计考量**：
- 极简配置。当前只有 `dasRoot` 一个必要参数。
- `enableDebugger` 预留字段——当下不实现，占位避免后续改接口。
- **不做的事情**：不在配置中定义模块搜索路径（daScript 通过 `dasRoot + "/daslib/"` 自行解析 `require`）。

### 3.2 DasEngine — 进程级脚本引擎

```cpp
// Src/Engine/DasEngine.h
#pragma once

#include "DasEngineConfig.h"
#include <memory>
#include <string>
#include <daScript/ast/ast.h>
#include <daScript/simulate/simulate.h>

namespace MMO
{
    /**
     * @brief 进程级 daScript 运行时封装
     * 
     * 每个进程调用一次 Initialize()，之后各服务器组件可通过
     * CompileScript()/CreateContext() 创建自己的脚本环境。
     * 
     * @note 非线程安全——Initialize/Shutdown 必须在进程主线程调用。
     */
    class DasEngine
    {
    public:
        static DasEngine &Instance();

        /**
         * @brief 初始化 daScript 运行时（进程生命周期内只调用一次）
         * - PULL_ALL_DEFAULT_MODULES
         * - das::Module::Initialize()
         * - das::setDasRoot()
         * @return 成功返回 true
         */
        bool Initialize(const DasEngineConfig &cfg);

        /**
         * @brief 关闭 daScript 运行时（进程退出前调用一次）
         * - das::Module::Shutdown()
         * - 清空内部缓存
         */
        void Shutdown();

        /**
         * @brief 编译脚本文件
         * @param entryFile  入口 .das 文件路径（相对于 dasRoot 或绝对路径）
         * @param libGroup   包含所有所需 daScript Module 的 ModuleGroup
         * @return 编译成功的 Program；失败返回 nullptr
         */
        das::ProgramPtr CompileScript(const std::string &entryFile,
                                      das::ModuleGroup   &libGroup);

        /**
         * @brief 从已编译的 Program 创建执行上下文
         * @param program  已编译的 Program
         * @return Context（shared_ptr 管理生命周期）
         */
        std::shared_ptr<das::Context> CreateContext(das::ProgramPtr program);

        /// 获取最后一次编译的错误信息
        const std::string &GetLastCompileErrors() const { return _lastCompileErrors; }

    private:
        DasEngine() = default;

        std::string _dasRoot;
        std::string _lastCompileErrors;
        bool        _initialized = false;
    };
}
```

```cpp
// Src/Engine/DasEngine.cpp
#include "Engine/DasEngine.h"

#include <daScript/simulate/fs_file_info.h>
#include <daScript/misc/string_writer.h>

// daScript 内建模块声明——每个链接了 DasEngine 的 target 必须包含此宏展开
DECLARE_ALL_DEFAULT_MODULES;

namespace MMO
{
    DasEngine &DasEngine::Instance()
    {
        static DasEngine engine;
        return engine;
    }

    bool DasEngine::Initialize(const DasEngineConfig &cfg)
    {
        if (_initialized)
        {
            return true; // 幂等
        }

        _dasRoot = cfg.dasRoot;
        das::setDasRoot(_dasRoot);

        PULL_ALL_DEFAULT_MODULES;
        das::Module::Initialize();

        _initialized = true;
        return true;
    }

    void DasEngine::Shutdown()
    {
        if (!_initialized) return;
        das::Module::Shutdown();
        _initialized = false;
    }

    das::ProgramPtr DasEngine::CompileScript(const std::string   &entryFile,
                                             das::ModuleGroup     &libGroup)
    {
        _lastCompileErrors.clear();

        auto fAccess = das::make_smart<das::FsFileAccess>();
        fAccess->introduceDaslib();

        das::TextWriter logs;
        auto program = das::compileDaScript(entryFile, fAccess, logs, libGroup);

        if (!program)
        {
            _lastCompileErrors = "program is null";
            return nullptr;
        }
        if (program->failed())
        {
            _lastCompileErrors = logs.str();
            return nullptr;
        }

        return program;
    }

    std::shared_ptr<das::Context> DasEngine::CreateContext(das::ProgramPtr program)
    {
        if (!program) return nullptr;
        return std::make_shared<das::Context>(program->getContextStackSize());
    }
}
```

**设计考量**：
1. **为什么不用 Context 池？** 当前单 LogicThread 架构下只有一个 Context 活跃。池化带来的复杂度和收益不成比例。如果未来需要 per-scene 并发脚本执行，再引入池化。
2. **为什么 CompileScript 和 CreateContext 分开？** Program 可被多个 Context 复用（如多 WorldServer 实例共享编译结果）。虽然目前只有一个 Context，但 API 分离不增加调用复杂度，却保留了未来的扩展空间。
3. **DECLARE_ALL_DEFAULT_MODULES 放在 .cpp 中**：避免每个 includer 都展开宏。

### 3.3 IScriptHost — 脚本宿主接口

这是整个方案中**最关键的设计决策**。

```cpp
// Src/Engine/IScriptHost.h
#pragma once

#include <cstdint>
#include <daScript/simulate/simulate.h>

namespace MMO
{
    /**
     * @brief 脚本宿主接口——任意服务器进程实现此接口即可接入脚本系统
     *
     * 自动生成的 Dispatch 函数通过此接口与具体服务器交互，
     * 不再绑定 WorldServer 类型。
     */
    class IScriptHost
    {
    public:
        virtual ~IScriptHost() = default;

        /// 获取 daScript 执行上下文
        virtual das::Context *GetScriptContext() const = 0;

        /// 获取 dispatch_msg 函数（脚本消息分发入口）
        virtual das::SimFunction *GetDispatchFunction() const = 0;

        /// 发送预序列化的 protobuf 消息到客户端
        virtual void SendRawToClient(uint32_t  sessionID,
                                     uint32_t  msgID,
                                     const uint8_t *data,
                                     size_t    len) = 0;
    };
}
```

**为什么需要这个接口**：

当前自动生成的代码（以 `Move.gen.cpp` 为例）：

```cpp
// ❌ 当前：硬编码 WorldServer
bool DispatchMoveReq(MMO::WorldServer &server, uint32 sessionID, ...) {
    das::Context *ctx = server.GetScriptContext();      // 只有 WorldServer 有
    auto fnDispatch = server.GetDispatchMsgFunction();  // 只有 WorldServer 有
}
```

当 SocialServer 也需要处理 `MoveReq` 或其他 protobuf 消息时，这段代码完全不适用。

**修正后**：

```cpp
// ✅ 修正后：多态接口
bool DispatchMoveReq(MMO::IScriptHost &host, uint32 sessionID, ...) {
    das::Context *ctx = host.GetScriptContext();      // 任意实现
    auto fnDispatch = host.GetDispatchFunction();     // 任意实现
}
```

### 3.4 BindingRegistry — 统一绑定注册

```cpp
// Src/Engine/Binding/BindingRegistry.h
#pragma once

namespace das
{
    class Module;
    class ModuleLibrary;
}

namespace MMO
{
    /**
     * @brief 脚本绑定注册总入口
     *
     * 替代当前分散的 RegisterDasBindings() + RegisterAllProtoMessageTypes()。
     * 各模块的 BindFunctions() 只需调用 RegisterAll() 即可。
     */
    class BindingRegistry
    {
    public:
        /**
         * @brief 注册全部 daScript 绑定
         * @note 在 MassiveModule::BindFunctions() 中调用一次。
         *       内部调用顺序：
         *         1. RegisterCoreBindings  — 非 Proto 的 C++ struct（BattleStats 等）
         *         2. RegisterAllProtoMessageTypes — Proto 消息类型（自动生成）
         */
        static void RegisterAll(das::Module &mod, das::ModuleLibrary &lib);
    };
}
```

```cpp
// Src/Engine/Binding/BindingRegistry.cpp
#include "Engine/Binding/BindingRegistry.h"

#include <daScript/daScriptModule.h>

// 前置声明：来自不同源文件的注册函数
namespace MMO
{
    // 核心类型绑定（迁移自 DasBindings.cpp）
    void RegisterCoreBindings(das::Module &mod, das::ModuleLibrary &lib);

    // Proto 消息类型注册（自动生成，在 ProtoBindIndex.gen.cpp 中定义）
    void RegisterAllProtoMessageTypes(das::Module &mod, das::ModuleLibrary &lib);
}

namespace MMO
{
    void BindingRegistry::RegisterAll(das::Module &mod, das::ModuleLibrary &lib)
    {
        RegisterCoreBindings(mod, lib);
        RegisterAllProtoMessageTypes(mod, lib);
    }
}
```

**设计考量**：
- `RegisterCoreBindings` 直接迁移自 `DasBindings.cpp` 中的 `RegisterDasBindings`，重命名，不改逻辑。
- `RegisterAllProtoMessageTypes` 由 `GenMsgBindings.py` 生成，已经存在于 `ProtoBindIndex.gen.cpp`，不需修改。
- 为什么不让 BindingRegistry 直接持有 ModuleLibrary？因为这会导致 BindingRegistry 与 Module 生命周期耦合。当前模式（外部创建 lib，传入）更清晰。

### 3.5 DasCoreBindings.cpp — 核心类型绑定迁移

从 `Src/Engine/DasBindings.cpp` 直接迁移，只改两处：

```
旧文件: Src/Engine/DasBindings.cpp
新文件: Src/Engine/Binding/DasCoreBindings.cpp

修改:
- 函数签名: void RegisterDasBindings(...) → void RegisterCoreBindings(...)
- include: 去掉不必要的 Engine 路径，加上 Binding 路径
- 函数体: 完全相同（14 个 getter + makeType + addExtern）
```

---

## 四、受影响文件的修改细节

### 4.1 WorldServer.h

```cpp
// 修改前（关键行）
class WorldServer
{
public:
    das::Context *GetScriptContext() const { return _scriptCtx.get(); }
    das::SimFunction *GetDispatchMsgFunction() const { return _fnDispatchMsg; }
    // ...

private:
    std::shared_ptr<das::Context>  _scriptCtx;
    das::ProgramPtr                _scriptProgram;
    das::SimFunction              *_fnInit = nullptr;
    das::SimFunction              *_fnUpdate = nullptr;
    das::SimFunction              *_fnDispatchMsg = nullptr;
    std::unique_ptr<MassiveModule> _massiveModule;
    uint64_t _lastGCHeapSize = 0;
};

// 修改后
#include "Engine/IScriptHost.h"

class WorldServer : public IScriptHost  // ← 新增继承
{
public:
    das::Context      *GetScriptContext()   const override;  // IScriptHost
    das::SimFunction  *GetDispatchFunction() const override; // IScriptHost
    void SendRawToClient(uint32 sessionID, uint32 msgID,
                         const uint8 *data, size_t len) override; // 已有，加 override

private:
    // 脚本相关成员——移入嵌套结构体以保持清晰
    struct ScriptHost
    {
        std::shared_ptr<das::Context>  ctx;
        das::ProgramPtr                program;
        das::SimFunction              *fnInit        = nullptr;
        das::SimFunction              *fnUpdate      = nullptr;
        das::SimFunction              *fnDispatchMsg = nullptr;
        uint64_t                       lastGCHeapSize = 0;
    };
    ScriptHost                     _scriptHost;
    std::unique_ptr<MassiveModule> _massiveModule;
};
```

**为什么用嵌套 struct 而不是拆散成员**：
- `ScriptHost` 作为一个概念整体，在 `Stop()` 中统一 reset
- 6 个成员变成 1 个成员，减少头文件视觉噪音
- `GetScriptContext()` 的实现变为 `return _scriptHost.ctx.get()`（一行改动）

### 4.2 WorldServer.cpp — InitScriptEngine()

```cpp
// 修改后的核心流程
bool WorldServer::InitScriptEngine()
{
    Log::Info("InitScriptEngine: starting...");

    // ── 1. 通过 DasEngine 编译脚本 ──
    auto &engine = DasEngine::Instance();  // 已在此前 Initialize 过
    
    _massiveModule = std::make_unique<MassiveModule>(
        this, &_sceneMgr, &_logicThread.GetTimingWheel(), &_sessions);
    _massiveModule->BindFunctions();  // 内部调用 BindingRegistry::RegisterAll()

    das::ModuleGroup libGroup;
    libGroup.addModule(_massiveModule.get());

    _scriptHost.program = engine.CompileScript("Script/ServerTick.das", libGroup);
    if (!_scriptHost.program)
    {
        Log::Error("InitScriptEngine: compile failed — {}",
                   engine.GetLastCompileErrors());
        return false;
    }

    // ── 2. 创建 Context ──
    _scriptHost.ctx = engine.CreateContext(_scriptHost.program);
    _massiveModule->_ctx = _scriptHost.ctx;

    // ── 3. Simulate + 查找函数 ──
    das::TextWriter simulateLogs;
    if (!_scriptHost.program->simulate(*_scriptHost.ctx, simulateLogs))
    {
        Log::Error("InitScriptEngine: simulate failed");
        return false;
    }

    // ── 4. Init / Update / dispatch_msg ──
    // （代码与旧版完全相同，仅变量名从 _scriptCtx → _scriptHost.ctx）
    auto fnInit = _scriptHost.ctx->findFunction("Init");
    if (fnInit) { /* ... */ _scriptHost.fnInit = fnInit; }
    
    _scriptHost.fnUpdate = _scriptHost.ctx->findFunction("Update");
    _scriptHost.fnDispatchMsg = _scriptHost.ctx->findFunction("dispatch_msg");
    if (_scriptHost.fnDispatchMsg) { RegisterAllMsgDispatch(); }

    return true;
}
```

### 4.3 WorldServer.cpp — OnTick()

```cpp
void WorldServer::OnTick(std::chrono::milliseconds elapsed)
{
    // ... 前面不变：ProcessUnroutedMessages, ProcessControlMessages, UpdateLoadLevel ...

    if (_scriptHost.fnUpdate && _scriptHost.ctx)
    {
        _scriptHost.ctx->restart();

        float dt = ...;
        vec4f args[] = { das::cast<uint32_t>::from(uint32_t(1)), das::cast<float>::from(dt) };
        _massiveModule->_scriptDt.store(dt, std::memory_order_relaxed);

        _scriptHost.ctx->evalWithCatch(_scriptHost.fnUpdate, args);
        // ... GC 逻辑（变量名 _lastGCHeapSize → _scriptHost.lastGCHeapSize）...

        auto *scene = _sceneMgr.GetDefaultScene();
        if (scene) { RunCPPSystems(*scene, dt, visibleSets); SystemReplicate(...); }
    }
}
```

**改动范围**：仅变量名重命名（`_scriptCtx` → `_scriptHost.ctx`，`_fnUpdate` → `_scriptHost.fnUpdate`，`_lastGCHeapSize` → `_scriptHost.lastGCHeapSize`）。

### 4.4 WorldServer.cpp — Stop()

```cpp
void WorldServer::Stop()
{
    _logicThread.Stop();
    // 先清空定时器——防止 Context 释放后回调
    if (_massiveModule) { _massiveModule->_timerCallbacks.clear(); }
    _scriptHost.ctx.reset();
    _scriptHost.program.reset();
    _massiveModule.reset();
    // ... 其余不变 ...
}
```

### 4.5 MassiveModule.cpp — BindFunctions()

```cpp
void MassiveModule::BindFunctions()
{
    g_massiveMod = this;

    ModuleLibrary lib(this);
    lib.addBuiltInModule();
    auto *builtin = Module::require("$");
    if (builtin) { addBuiltinDependency(lib, builtin, true); }

    // ── 15 个桥接函数注册 ──
    addExtern<DAS_BIND_FUN(Bridge_EntityPosition)>(*this, lib, "EntityPosition", ...);
    addExtern<DAS_BIND_FUN(Bridge_EntityIsDead)>(*this, lib, "EntityIsDead", ...);
    // ... 其余 13 个桥接函数不变 ...

    addExtern<DAS_BIND_FUN(Bridge_LogError)>(*this, lib, "LogError", ...);

    // ── 统一绑定入口（替代手写 RegisterAllProtoMessageTypes + RegisterDasBindings）──
    BindingRegistry::RegisterAll(*this, lib);  // ← 这就是改动
}
```

### 4.6 ScriptDispatchRegistry

```cpp
// ScriptDispatchRegistry.h — 修改前
static bool Dispatch(WorldServer &server, uint32 sessionID, uint32 msgID,
                     const uint8 *body, size_t len);

// ScriptDispatchRegistry.h — 修改后
class IScriptHost; // 前向声明
static bool Dispatch(IScriptHost &host, uint32 sessionID, uint32 msgID,
                     const uint8 *body, size_t len);
```

```cpp
// ScriptDispatchRegistry.cpp — 修改前
bool ScriptDispatchRegistry::Dispatch(WorldServer &server, ...) { ... }

// ScriptDispatchRegistry.cpp — 修改后
bool ScriptDispatchRegistry::Dispatch(IScriptHost &host, ...) { ... }
```

```cpp
// WorldServer.cpp — OnMessage() 调用处修改前
if (ScriptDispatchRegistry::Dispatch(*this, sessionID, msg.msgID, ...))

// WorldServer.cpp — OnMessage() 调用处修改后
// 完全相同——WorldServer* 隐含转为 IScriptHost&，无需改动
if (ScriptDispatchRegistry::Dispatch(*this, sessionID, msg.msgID, ...))
```

### 4.7 GenMsgBindings.py

需要修改的位置在 `generate_proto_gen_cpp()` 函数中（约第 315 行和第 459 行）：

```python
# 修改 1：include 路径
# 旧：
lines.append(f'#include "World/ScriptDispatchRegistry.h"')
lines.append(f'#include "World/WorldServer.h"')
# 新：
lines.append(f'#include "World/ScriptDispatchRegistry.h"')
lines.append(f'#include "Engine/IScriptHost.h"')

# 修改 2：Dispatch 函数签名
# 旧：
lines.append(f"    bool {func_name}(MMO::WorldServer &server, uint32 sessionID, ...")
# 新：
lines.append(f"    bool {func_name}(MMO::IScriptHost &host, uint32 sessionID, ...")

# 修改 3：Dispatch 函数体内——局部变量
# 旧：
lines.append(f"        das::Context *ctx        = server.GetScriptContext();")
lines.append(f"        auto          fnDispatch = server.GetDispatchMsgFunction();")
# 新：
lines.append(f"        das::Context *ctx        = host.GetScriptContext();")
lines.append(f"        auto          fnDispatch = host.GetDispatchFunction();")
```

**重新生成命令**：
```bash
python Tools/Script/GenMsgBindings.py \
    --proto-dir Src/Proto \
    --cpp-out Src/World/AutoGen \
    --das-out Script/AutoGen
```

---

## 五、xmake 构建变更

### 5.1 新增 `Src/Engine/xmake.lua`

```lua
--- @file xmake.lua
--- @brief 脚本引擎封装层静态库——被各服务器 target 链接

target("Engine")
    set_kind("static")
    add_files("**.cpp")
    add_headerfiles("*.h")

    add_deps(
        "CommonCore",
        "CommonLog",
        "Proto"  -- MAKE_TYPE_FACTORY 需要 protobuf 生成的 .pb.h
    )
```

### 5.2 修改 `Src/World/xmake.lua`

```lua
-- 在 add_deps 中添加
add_deps(
    "Engine",       -- ← 新增
    "CommonCore",
    "CommonDB",
    -- ... 其余不变 ...
)
```

### 5.3 修改根 `xmake.lua`

```lua
-- 在 includes 中添加
includes("Src/Engine/xmake.lua")   -- ← 新增（放在 Common 之前）
includes("Src/Common/xmake.lua")
-- ... 其余不变 ...
```

### 5.4 `DasBindings.cpp` 的迁移

`Src/Engine/DasBindings.cpp` 从 Engine target 中移除（文件删除），其内容迁移到 `Src/Engine/Binding/DasCoreBindings.cpp`。

注意：`MassiveModule.cpp` 当前在 `Src/World/` 下，它会 include `DasBindings.cpp` 中定义的 `RegisterDasBindings`。迁移后 `MassiveModule.cpp` 应改为通过 `BindingRegistry::RegisterAll()` 间接调用 `RegisterCoreBindings()`，不再直接 include 绑定实现。

---

## 六、实施步骤

### Step 1: Engine 静态库 + DasEngine（1.5 天）

```
创建:
  Src/Engine/xmake.lua
  Src/Engine/DasEngineConfig.h
  Src/Engine/DasEngine.h
  Src/Engine/DasEngine.cpp
  Src/Engine/IScriptHost.h

修改:
  xmake.lua (root)             —— 加 includes("Src/Engine/xmake.lua")
  Src/World/xmake.lua          —— 加 add_deps("Engine")
```

**验证**：项目编译通过（link 到 Engine 库，但没有调用）。

### Step 2: WorldServer 适配 IScriptHost（1 天）

```
修改:
  Src/World/WorldServer.h      —— : public IScriptHost + ScriptHost 结构体
  Src/World/WorldServer.cpp    —— InitScriptEngine()/OnTick()/Stop() 变量重命名
  Src/World/MassiveModule.cpp  —— DasEngine::Instance().Initialize() 调用
```

**验证**：WorldServer 编译 + 运行，烟雾测试通过。

### Step 3: Binding 迁移 + Registry（0.5 天）

```
创建:
  Src/Engine/Binding/BindingRegistry.h
  Src/Engine/Binding/BindingRegistry.cpp
  Src/Engine/Binding/DasCoreBindings.cpp  (从 DasBindings.cpp 迁移)
```

**验证**：MassiveModule 调用 `BindingRegistry::RegisterAll()` → 编译通过。

### Step 4: GenMsgBindings.py 解耦 + 重新生成（0.5 天）

```
修改:
  Tools/Script/GenMsgBindings.py  —— WorldServer& → IScriptHost&（3 处）
  Src/World/ScriptDispatchRegistry.h   —— Dispatch 签名
  Src/World/ScriptDispatchRegistry.cpp —— 同上

重新生成:
  python Tools/Script/GenMsgBindings.py ...
```

**验证**：全量编译 + 消息分发端到端测试。

### Step 5: 清理旧代码（0.5 天）

```
删除:
  Src/Engine/DasBindings.cpp

清理:
  WorldServer::CompileDaScript() —— 已有 DasEngine::CompileScript() 替代
     （如果 CompileDaScript 没有被其他地方引用，可删除；否则保留为兼容层）
```

---

## 七、代码量估算

| 阶段 | 新增代码 | 修改代码 | 删除代码 | 净增 |
|------|----------|----------|----------|------|
| Step 1 | ~290 行 | ~5 行 | 0 | +295 |
| Step 2 | ~30 行 | ~60 行 | 0 | +90 |
| Step 3 | ~180 行 | ~5 行 | ~110 行 | +75 |
| Step 4 | 0 | ~10 行 | 0 | +10 |
| Step 5 | 0 | ~5 行 | ~5 行 | 0 |
| **合计** | **~500 行** | **~85 行** | **~115 行** | **~470 行** |

---

## 八、SocialServer 脚本化（第二阶段预览）

Phase 2 实施 SocialServer 脚本化时，需要：

```
新增:
  Src/Social/SocialServer.h/.cpp  —— : public IScriptHost
  Src/Social/SocialModule.h/.cpp  —— 社交业务桥接函数
  Src/Social/main.cpp             —— 进程入口 + DasEngine::Instance().Initialize()
  Script/Social/ServerTick.das    —— 社交脚本入口
```

SocialServer 的 `InitScriptEngine()` 与 WorldServer 的流程**80% 相同**——这就是引入 `DasEngine` 的收益验证点。

---

## 九、不做的事情（明确边界）

以下功能**刻意排除**在本次重构之外：

| 功能 | 排除原因 | 替代方案 |
|------|----------|----------|
| Context 池 | 单线程架构无并发需求 | 保持单个 `shared_ptr<Context>` |
| 热重载 | daScript 不支持类型热替换 | 进程重启（开发期用 `das::restart()` 回卷栈已够用） |
| Snapshot/Restore | 脚本当前无持久状态需要序列化 | 无替代——等业务需要时再做 |
| AOT Pipeline | 脚本量 < 320 行，JIT 足够 | 脚本超过 10,000 行后再考虑 |
| clang-based BindingGen | 独立大工程 (~3000 行) | 继续用成熟的 Python 生成器 |
| ScriptModuleManager | Module 生命周期由 WorldServer 直接管理 | 无替代——复杂度收益比不佳 |
| ServerCtl 的调试命令 | 不在本次范围 | 后续通过 `DasEngine::Instance()` 可加 |

---

## 十、风险与回退

| 风险 | 概率 | 缓解措施 |
|------|------|----------|
| `IScriptHost` 引入虚函数 | 低 | 3 个虚函数 = 1 个 vtable 指针（8 bytes），无性能影响 |
| GenMsgBindings.py 改动遗漏 | 中 | Step 4 重新生成后 git diff 人工审查全部 `.gen.cpp` |
| DECLARE_ALL_DEFAULT_MODULES 重复定义 | 低 | 放在 `DasEngine.cpp` 中（一个 translation unit） |
| BindingRegistry 初始化顺序 | 低 | `RegisterCoreBindings` 不依赖 `RegisterAllProtoMessageTypes`，顺序不影响 |

**回退方案**：如果 Step 1-2 后出问题，只需 revert `WorldServer.h/cpp` 中变量重命名的部分。Binding 迁移和 IScriptHost 解耦都是正交修改，可独立回退。

---

## 附录 A：完整文件依赖图

```
DasEngine.cpp ───── DasEngineConfig.h
                    IScriptHost.h (独立)

BindingRegistry.cpp ── DasCoreBindings.cpp (RegisterCoreBindings)
                       ProtoBindIndex.gen.cpp (RegisterAllProtoMessageTypes)

MassiveModule.cpp ──── BindingRegistry.h
                       DasHelpers.h

WorldServer.cpp ────── DasEngine.h
                       IScriptHost.h (继承)
                       BindingRegistry.h (通过 MassiveModule)

ScriptDispatchRegistry.cpp ── IScriptHost.h

Move.gen.cpp ──────── IScriptHost.h (生成代码)
Login.gen.cpp ─────── IScriptHost.h (生成代码)
```

## 附录 B：调用时序

```
main()
 ├─ DasEngine::Instance().Initialize(config)    ← 进程级，一次
 ├─ WorldServer::Init()
 │   ├─ DasEngine::Instance().CompileScript("Script/ServerTick.das", libGroup)
 │   ├─ DasEngine::Instance().CreateContext(program)
 │   ├─ program->simulate(ctx)
 │   └─ ctx->findFunction("Init/Update/dispatch_msg")
 │
 └─ (LogicThread)
     ├─ ctx->restart()
     ├─ ctx->evalWithCatch(fnUpdate, args)
     ├─ RunCPPSystems()
     └─ SystemReplicate()

 (消息到达)
 └─ ScriptDispatchRegistry::Dispatch(host, sessionID, msgID, ...)
     ├─ host.GetScriptContext()     ← IScriptHost vtable
     ├─ host.GetDispatchFunction()  ← IScriptHost vtable
     └─ ctx->eval(fnDispatch, callArgs)
```
