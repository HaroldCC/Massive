# Massive Module 分层架构设计报告

> **版本**: v1.0  
> **日期**: 2026-07-29  
> **分析范围**: `Src/ScriptEngine/`、`Src/World/MassiveModule.cpp` 中全部 daScript 绑定  
> **目标**: 将当前扁平化的 "massive" Module 拆分为有层次、可复用、按职责分层的模块体系

---

## 一、当前状态（磁盘真实状态）

> ⚠️ 不存在 `Src/Engine/`、`DasBindings.cpp`。以下均为实际读取到的文件。

### 1.1 ScriptEngine 层（已有一版实现）

| 文件 | 内容 |
|------|------|
| `Src/ScriptEngine/DasEngine.h` | `DasLangEngine` 单例 — `Initialize/Shutdown/CompileScript/CreateContext` |
| `Src/ScriptEngine/DasEngine.cpp` | 实现 — `DECLARE_ALL_DEFAULT_MODULES`、`PULL_ALL_DEFAULT_MODULES`、`compileDaScript` |
| `Src/ScriptEngine/DasEngineConfig.h` | `DasLangEngineConfig { dasLangRoot, enableDebugger }` |
| `Src/ScriptEngine/IDasHost.h` | `IDasLangtHost` 接口 — `GetScriptContext/GetDispatchFunc/SendRawToClient` |
| `Src/ScriptEngine/DasHelpers.h` | `CreateDasArrayFromVector<T>()` 模板 |
| `Src/ScriptEngine/xmake.lua` | `ScriptEngine` 静态库 — `add_deps("CommonCore", "libDaScript")` |

### 1.2 WorldServer 已实现 IDasLangtHost

```cpp
// Src/World/WorldServer.h
#include "ScriptEngine/IDasHost.h"
class WorldServer : public IDasLangtHost { ... };

// 已内嵌 DasLangHost 结构体：
struct DasLangHost {
    std::shared_ptr<das::Context> _scriptCtx;
    das::ProgramPtr               _scriptProgram;
    das::SimFunction             *_fnInit = nullptr;
    das::SimFunction             *_fnUpdate = nullptr;
    das::SimFunction             *_fnDispatchMsg = nullptr;
} _dasHost;
```

### 1.3 所有 daScript 注册集中在一个 Module 中

**唯一入口**：`Src/World/MassiveModule.cpp` — `BindFunctions()` 方法  

当前所有注册全部塞进 **一个** `das::Module("massive")`：

```
Module "massive"
├── LogInfo / LogWarn / LogError           ← 通用日志（任何服务器都需要）
├── ScheduleTimer / CancelTimer            ← 通用定时器
├── GetDeltaTime                           ← 通用时间
├── SendToClient                           ← 通用消息发送
├── EntityPosition                         ← World 专属（ECS 空间查询）
├── EntityIsDead/InCombat/Stunned/Player/Monster  ← World 专属（ECS Tag）
├── CreateEntity / DestroyEntity           ← World 专属（ECS 世界交互）
├── FindEntityBySession                    ← World 专属（Session→Entity 映射）
├── RegisterAllProtoMessageTypes           ← Proto 类型注册（自动生成）
│   ├── Vector3, ErrorInfo
│   ├── HeartbeatReq/Rsp, LoginAuthReq/Rsp, ...
│   └── MoveReq/Rsp
└── EMsgID enum                            ← 枚举注册
```

---

## 二、问题诊断

### 2.1 复用性问题

SocialServer 想引入脚本只需要：
- `LogInfo/LogWarn/LogError`
- `ScheduleTimer/CancelTimer`
- `GetDeltaTime`
- `SendToClient`（消息发送）
- 它自己的 Proto 类型（好友、公会、聊天消息）

但当前它必须**不要**以下内容：
- `EntityPosition`、`CreateEntity`、`EntityIsDead`……（ECS 操作）
- `FindEntityBySession`（WorldSession 映射）
- `HeartbeatReq`、`MoveReq`……（World 侧的消息类型）

### 2.2 编译依赖问题

当前 `MassiveModule.cpp` 的 include 链：

```
MassiveModule.cpp
├── World/Component/BattleStats.h     ← SocialServer 不需要
├── World/Component/EntityType.h      ← SocialServer 不需要
├── World/Component/Health.h          ← SocialServer 不需要
├── World/Component/Position.h        ← SocialServer 不需要
├── World/Component/Tags.h            ← SocialServer 不需要
├── World/Component/Velocity.h        ← SocialServer 不需要
├── World/SceneManager.h              ← SocialServer 不需要
├── World/WorldServer.h               ← SocialServer 不需要
├── World/WorldSession.h              ← SocialServer 不需要
└── Common/ECS/Scene.h                ← SocialServer 不需要
```

SocialServer 如果直接复用 `MassiveModule`，会引入 ECS/World 的**全部编译依赖**。

### 2.3 脚本可见性问题

当前脚本 `require massive` 后可以调用任何注册函数。SocialServer 的脚本如果 `require massive`，IDE 会提示 `CreateEntity` 等 ECS 函数，但这些在 SocialServer 进程中**根本不可用**——运行时调用必 crash。

### 2.4 职责边界模糊

`MassiveModule` 的构造函数需要 4 个指针：
```cpp
MassiveModule(WorldServer*, SceneManager*, TimingWheel*, Sessions*)
```
这 4 个指针决定了它能做什么。SocialServer 有完全不同的上下文对象。

---

## 三、分层设计

### 3.1 模块分层全景

```
┌─────────────────────────────────────────────────────────┐
│                    脚本层 (Script/*.das)                 │
│  require core          — 任何服务器都可用                │
│  require world         — 仅 WorldServer 可用             │
│  require social        — 仅 SocialServer 可用            │
│  require AutoGen/...   — 自动生成的 Proto 类型           │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────┼──────────────────────────────────┐
│              daScript Module 分层                        │
│                                                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │ "core"       │  │ "world"      │  │ "social"     │  │
│  │ (DasCoreMod) │  │ (DasWorldMod)│  │ (DasSocialMod│  │
│  │              │  │              │  │              │  │
│  │ LogInfo      │  │ EntityPos    │  │ FriendAdd    │  │
│  │ LogWarn      │  │ EntityIsDead │  │ GuildCreate  │  │
│  │ LogError     │  │ CreateEntity │  │ ChatSend     │  │
│  │ ScheduleTimer│  │ DestroyEntity│  │ ...          │  │
│  │ CancelTimer  │  │ FindBySess   │  │              │  │
│  │ GetDeltaTime │  │ ...          │  │              │  │
│  │ SendToClient │  │              │  │              │  │
│  │ BattleStats  │  │ [依赖 core]  │  │ [依赖 core]  │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│                                                         │
│  ┌──────────────────────────────────────────────────┐   │
│  │ Proto 类型注册 (自动生成，按 Proto 文件拆分)      │   │
│  │ Common.gen.cpp → Vector3, ErrorInfo              │   │
│  │ Login.gen.cpp  → HeartbeatReq, LoginAuthReq, ... │   │
│  │ Move.gen.cpp   → MoveReq, MoveRsp               │   │
│  │ ProtoBindIndex.gen.cpp → 汇总入口                │   │
│  │ EMsgID 枚举 → 每个服务器共享                    │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### 3.2 文件分布方案

```
Src/ScriptEngine/                    ← 已有，不新增目录
├── DasEngine.h / .cpp               ← 已有
├── DasEngineConfig.h                ← 已有
├── DasHelpers.h                     ← 已有
├── IDasHost.h                       ← 已有（实现 IDasLangtHost 接口）
├── xmake.lua                        ← 已有

Src/ScriptEngine/Module/             ← 新增目录
├── DasCoreModule.h                  ← Core Module 头文件
├── DasCoreModule.cpp                ← Core Module 实现
│   (LogInfo/Warn/Error, ScheduleTimer, CancelTimer, 
│    GetDeltaTime, SendToClient, BattleStats+访问器)

Src/World/                           ← 已有目录
├── MassiveModule.h                  ← 保留，重命名考虑: DasWorldModule.h？
├── MassiveModule.cpp                ← 重构：只剩 ECS 相关函数
│   (EntityPosition, EntityIs*, CreateEntity, DestroyEntity, FindEntityBySession)

Src/World/AutoGen/                   ← 已有（自动生成）
├── ProtoBindIndex.gen.h/cpp         ← 汇总入口
├── Common.gen.cpp                   ← Proto 类型注册
├── Login.gen.cpp                    ← Proto 类型注册 + Dispatch
├── Move.gen.cpp                     ← Proto 类型注册 + Dispatch
```

### 3.3 Core Module 设计

```cpp
// Src/ScriptEngine/Module/DasCoreModule.h
#pragma once

#include <daScript/daScriptModule.h>
#include <memory>
#include <functional>

namespace MMO
{
    class IDasLangtHost;  // 前向声明
    class TimingWheel;

    /**
     * @brief daScript Module "core" — 所有服务器共享的基础功能
     *
     * 注册内容: 日志、定时器、deltaTime、消息发送、BattleStats
     * 依赖: 仅 CommonCore / libDaScript，不依赖任何 World/Social 代码
     *
     * 构造函数接收最通用的接口:
     *   - host: IDasLangtHost (GetScriptContext, SendRawToClient)
     *   - timingWheel: TimingWheel (定时器)
     */
    class DasCoreModule : public das::Module
    {
    public:
        DasCoreModule();
        ~DasCoreModule() override;

        /**
         * @brief 设置上下文——BindFunctions 前必须调用
         */
        void SetHost(IDasLangtHost *host, TimingWheel *timingWheel);

        /**
         * @brief 注册所有绑定函数
         */
        void BindFunctions();

        // ── 上下文访问 ──
        das::Context *GetContext() const { return _ctx.get(); }
        void          SetContext(std::shared_ptr<das::Context> ctx) { _ctx = std::move(ctx); }
        std::atomic<float> &ScriptDt() { return _scriptDt; }

        // ── 定时器回调表 ──
        struct TimerCallback
        {
            das::TBlock<void, uint32_t>   block;
            std::shared_ptr<das::Context> ctx;
        };
        std::unordered_map<uint32_t, TimerCallback> _timerCallbacks;
        std::atomic<uint32_t>                       _nextTimerID{1};

    private:
        IDasLangtHost *_host         = nullptr;
        TimingWheel   *_timingWheel  = nullptr;
        std::shared_ptr<das::Context> _ctx;
        std::atomic<float>            _scriptDt{0.02f};
    };
}
```

```cpp
// Src/ScriptEngine/Module/DasCoreModule.cpp
// 注册内容:
//
// addExtern:  LogInfo, LogWarn, LogError
// addExtern:  ScheduleTimer, CancelTimer
// addExtern:  GetDeltaTime
// addExtern:  SendToClient
// makeType:   BattleStats
// addExtern:  BattleStats_Attack, BattleStats_Defense, ... (14 个访问器)
//
// 双注册宏:
// REGISTER_DYN_MODULE(DasCoreModule, DasCoreModule);
// REGISTER_MODULE(DasCoreModule);
```

### 3.4 World Module 设计（重构后的 MassiveModule）

```cpp
// Src/Common/ECS/MassiveModule.h — 精简版
// （建议后续重命名为 DasWorldModule，但先保持文件名避免 churn）

namespace MMO
{
    class WorldServer;
    class SceneManager;

    /**
     * @brief daScript Module "world" — ECS 实体操作
     *
     * 依赖 "core" Module (addBuiltinDependency)
     * 构造需要: WorldServer + SceneManager + Sessions
     * 脚本侧: require world (或 require massive，名称过渡期间保持兼容)
     */
    class MassiveModule : public das::Module
    {
    public:
        MassiveModule(WorldServer*, SceneManager*, 
                      std::unordered_map<uint32, WorldSession>*);

        void BindFunctions();

        // _ctx 和 _scriptDt 委托给 DasCoreModule
        // （World 模块不自己管理 Context，而是共享 CoreModule 的）
        void SetCoreModule(DasCoreModule *coreMod) { _coreMod = coreMod; }

    private:
        DasCoreModule *_coreMod = nullptr;  // 共享的 Core Module
        WorldServer   *_worldServer;
        SceneManager  *_sceneMgr;
        std::unordered_map<uint32, WorldSession> *_sessions;
    };
}
```

World Module 注册内容（从 MassiveModule.cpp 中保留）：

```
addExtern: EntityPosition
addExtern: EntityIsDead, EntityIsInCombat, EntityIsStunned
addExtern: EntityIsPlayer, EntityIsMonster
addExtern: CreateEntity, DestroyEntity
addExtern: FindEntityBySession
```

### 3.5 Module 间依赖关系

在 `InitScriptEngine()` 中的注册顺序：

```cpp
bool WorldServer::InitScriptEngine()
{
    DasLangEngine::GetIns().Initialize(cfg);

    // ── Step 1: 创建 Core Module（所有服务器共享）──
    _coreModule = std::make_unique<DasCoreModule>();
    _coreModule->SetHost(this, &_logicThread.GetTimingWheel());
    _coreModule->BindFunctions();

    // ── Step 2: 创建 World Module（依赖 Core）──
    _worldModule = std::make_unique<MassiveModule>(this, &_sceneMgr, &_sessions);
    _worldModule->SetCoreModule(_coreModule.get());
    _worldModule->BindFunctions();

    // ── Step 3: ModuleGroup ──
    das::ModuleGroup libGroup;
    libGroup.addModule(_coreModule.get());
    libGroup.addModule(_worldModule.get());

    // ── Step 4: 编译入口脚本 ──
    _dasHost._scriptProgram = DasLangEngine::GetIns()
        .CompileScript("Script/ServerTick.das", libGroup);

    // ── Step 5: Context 注入 ──
    _dasHost._scriptCtx = DasLangEngine::GetIns()
        .CreateContext(_dasHost._scriptProgram);
    _coreModule->SetContext(_dasHost._scriptCtx);

    // ── Step 6: simulate + findFunction ──
    // ...
}
```

### 3.6 脚本侧的 require

```dascript
// Script/ServerTick.das — WorldServer 入口
options gen2
require core          // ← 拿到 Log/Timer/DeltaTime/SendToClient/BattleStats
require world         // ← 拿到 EntityPosition/CreateEntity 等 ECS 操作
require AutoGen/HandlerRegistry public  // ← Proto 类型 + dispatch_msg

// Script/Social/ServerTick.das — 未来 SocialServer 入口
options gen2
require core          // ← 同样的基础功能
require social        // ← 拿到 好友/公会/聊天 操作（不包含 ECS）
require AutoGen/SocialHandlerRegistry public
```

---

## 四、函数归属明细

### 4.1 Core Module (`DasCoreModule`) — 17 个注册项

| 脚本函数名 | C++ 桥接函数 | 来源 | 备注 |
|-----------|-------------|------|------|
| `LogInfo` | `Bridge_LogInfo` | MassiveModule.cpp | 通用 |
| `LogWarn` | `Bridge_LogWarn` | MassiveModule.cpp | 通用 |
| `LogError` | `Bridge_LogError` | MassiveModule.cpp | 通用 |
| `ScheduleTimer` | `Bridge_ScheduleTimer` | MassiveModule.cpp | 通用，依赖 TimingWheel |
| `CancelTimer` | `Bridge_CancelTimer` | MassiveModule.cpp | 通用 |
| `GetDeltaTime` | `Bridge_GetDeltaTime` | MassiveModule.cpp | 通用 |
| `SendToClient` | `Bridge_SendToClient` | MassiveModule.cpp | 通用，依赖 IDasLangtHost |
| `BattleStats` (类型) | `makeType<BattleStats>(lib)` | DasBindings.cpp* | 通用 POD，任意服务器可读取战斗属性 |
| `BattleStats_Attack` | `BattleStats_GetAttack` | DasBindings.cpp* | 字段访问器 |
| `BattleStats_Defense` | `BattleStats_GetDefense` | DasBindings.cpp* | 字段访问器 |
| `BattleStats_MagicAttack` | ... | DasBindings.cpp* | 字段访问器 |
| `BattleStats_MagicDefense` | ... | DasBindings.cpp* | 字段访问器 |
| `BattleStats_CritRate` | ... | DasBindings.cpp* | 字段访问器 |
| `BattleStats_CritDamage` | ... | DasBindings.cpp* | 字段访问器 |
| `BattleStats_DodgeRate` | ... | DasBindings.cpp* | 字段访问器 |
| `BattleStats_HitRate` | ... | DasBindings.cpp* | 字段访问器 |
| `BattleStats_AttackSpeed` | ... | DasBindings.cpp* | 字段访问器 |
| `BattleStats_MoveSpeed` | ... | DasBindings.cpp* | 字段访问器 |
| `BattleStats_CurrentHp` | ... | DasBindings.cpp* | 字段访问器 |
| `BattleStats_MaxHp` | ... | DasBindings.cpp* | 字段访问器 |
| `BattleStats_CurrentMp` | ... | DasBindings.cpp* | 字段访问器 |
| `BattleStats_MaxMp` | ... | DasBindings.cpp* | 字段访问器 |

> \* DasBindings.cpp 在磁盘上不存在——这些注册代码目前实际上**不编译**。如果从未被调用过，说明当前脚本也没用到 BattleStats 字段读取。需要确认。

### 4.2 World Module (`MassiveModule` 精简后) — 10 个注册项

| 脚本函数名 | C++ 桥接函数 | 来源 | 备注 |
|-----------|-------------|------|------|
| `EntityPosition` | `Bridge_EntityPosition` | MassiveModule.cpp | ECS 空间查询 |
| `EntityIsDead` | `Bridge_EntityIsDead` | MassiveModule.cpp | ECS Tag 判断 |
| `EntityIsInCombat` | `Bridge_EntityIsInCombat` | MassiveModule.cpp | ECS Tag 判断 |
| `EntityIsStunned` | `Bridge_EntityIsStunned` | MassiveModule.cpp | ECS Tag 判断 |
| `EntityIsPlayer` | `Bridge_EntityIsPlayer` | MassiveModule.cpp | ECS Tag 判断 |
| `EntityIsMonster` | `Bridge_EntityIsMonster` | MassiveModule.cpp | ECS Tag 判断 |
| `CreateEntity` | `Bridge_CreateEntity` | MassiveModule.cpp | ECS 世界交互 |
| `DestroyEntity` | `Bridge_DestroyEntity` | MassiveModule.cpp | ECS 世界交互 |
| `FindEntityBySession` | `Bridge_FindEntityBySession` | MassiveModule.cpp | Session 映射 |
| `EntityGetBattleStats` | `Bridge_EntityGetBattleStats` | MassiveModule.cpp | (已注释掉) |

### 4.3 Proto Type Module（自动生成）— 按 .proto 文件拆分

| 文件 | 注册的类型 | 是否跨服务器共享 |
|------|----------|:---:|
| `Common.gen.cpp` | `Vector3`, `ErrorInfo` | ✅ 是 |
| `Login.gen.cpp` | `HeartbeatReq/Rsp`, `LoginAuthReq/Rsp`, `LoginEnterWorldReq/Rsp` | ✅ 是 |
| `Move.gen.cpp` | `MoveReq/Rsp` | ✅ 是 |
| `EMsgID` 枚举绑定 | `MSG_HEARTBEAT_REQ` ~ `MSG_ENTITY_DESPAWN_NTF` | ✅ 是 |

> Proto 类型当前全部注册在 World Module 中。分层后，建议 Proto 类型也拆出去——但这是后话，不影响本次重构。

---

## 五、SocialServer 接入时的收益验证

假设 SocialServer 按分层方案实施：

```cpp
// Src/Social/SocialServer.cpp
bool SocialServer::InitScriptEngine()
{
    // Step 1: Core Module（完全复用）
    _coreModule = std::make_unique<DasCoreModule>();
    _coreModule->SetHost(this, &_timingWheel);
    _coreModule->BindFunctions();

    // Step 2: Social Module（新写，只含社交业务函数）
    _socialModule = std::make_unique<DasSocialModule>(this, &_friendMgr, &_guildMgr);
    _socialModule->BindFunctions();

    // Step 3: ModuleGroup — 不包含 World Module
    das::ModuleGroup libGroup;
    libGroup.addModule(_coreModule.get());
    libGroup.addModule(_socialModule.get());

    // Step 4-6: 编译/Context/simulate — 完全复用 DasLangEngine
    // ...
}

// 依赖链: SocialServer → ScriptEngine(Core) + SocialModule
// 不依赖: ECS, Scene, EntityPosition, EnTT 任何东西
```

**核心收益**：
- SocialServer **不链接** EnTT/ECS/World 任何代码
- **不需要** `require world` → 不会有无法使用的 ECS 函数出现在 IDE 补全中
- Core Module 的 17 个注册项**零成本复用**（只是一次 `std::make_unique<DasCoreModule>()`）

---

## 六、实施建议

### 6.1 阶段划分

| 阶段 | 内容 | 工作量 | 风险 |
|------|------|--------|------|
| **Phase A** | 创建 `DasCoreModule`，从 `MassiveModule` 中**迁移** Log/Timer/DeltaTime/SendToClient | 1 天 | 低 |
| **Phase B** | 精简 `MassiveModule` 为纯 World 函数，添加 `SetCoreModule()` | 0.5 天 | 低 |
| **Phase C** | 修改 `InitScriptEngine()` 创建 Core+World 两个 Module | 0.5 天 | 中（需同步更新 `ServerTick.das` 的 require） |
| **Phase D** | 确认 BattleStats 绑定存在性→迁移到 CoreModule | 0.5 天 | 低 |
| **Phase E** | 更新 GenMsgBindings.py 生成 `IDasLangtHost&` 接口 | 0.5 天 | 中 |

### 6.2 兼容性策略

`ServerTick.das` 当前 `require massive`，重构后需按以下方式过渡：

```dascript
// 过渡期：同时 require，验证功能
require core
require massive    // ← 旧名，仍可用（MassiveModule 的 daScript Module 名改为 "massive" 保留）

// 完全迁移后：
require core
require world      // ← MassiveModule 改名为 "world"
```

daScript 的 `Module` 名称与 C++ 类名无关——可以在不改文件名的情况下先把 daScript 侧的名称改为 `"world"`：

```cpp
// MassiveModule 构造函数中
MassiveModule::MassiveModule(...) : Module("world")  // daScript 侧叫 "world"
```

### 6.3 不需要做的事情

| 事项 | 原因 |
|------|------|
| 改名 `MassiveModule` → `DasWorldModule` | 可以等 Core 拆分稳定后再改名，避免一次 commit 波及太多文件 |
| Proto 类型拆分为独立 Module | 当前 Proto 类型注册进哪个 Module 都可以（daScript 类型系统是全局的）。分层以后如果发现污染问题再拆 |
| `ScriptDispatchRegistry` 拆分 | 当前只有 6 个 msgID，按服务器拆分为时过早 |

---

## 七、文件变更清单

### 新增文件

| 文件 | 说明 |
|------|------|
| `Src/ScriptEngine/Module/DasCoreModule.h` | Core Module 头文件 |
| `Src/ScriptEngine/Module/DasCoreModule.cpp` | Core Module 实现（~250 行，含 BattleStats 访问器） |

### 修改文件

| 文件 | 修改内容 |
|------|----------|
| `Src/ScriptEngine/xmake.lua` | 添加 `Module/**.cpp` 文件 |
| `Src/World/MassiveModule.cpp` | 移除 Log/Timer/DeltaTime/SendToClient 注册代码 |
| `Src/World/MassiveModule.cpp` | 添加 `SetCoreModule()` |
| `Src/World/WorldServer.h` | 新增 `_coreModule` 成员 (`std::unique_ptr<DasCoreModule>`) |
| `Src/World/WorldServer.cpp` | `InitScriptEngine()` 改为创建 `DasCoreModule` + `MassiveModule` 双模块 |
| `Src/World/WorldServer.cpp` | `OnTick()` 中 `_scriptDt` 从 `_coreModule->ScriptDt()` 读取 |
| `Script/ServerTick.das` | 改为 `require core` + `require massive`（过渡期） |
| `Script/Handlers.das` | 改为 `require core`（如果用到 Log 等） |

### 不变的

| 文件 | 原因 |
|------|------|
| `Src/ScriptEngine/DasEngine.*` | 已有正确实现，本次不涉及 |
| `Src/ScriptEngine/IDasHost.h` | 已有 `IDasLangtHost`，本次不涉及 |
| `Src/ScriptEngine/DasHelpers.h` | 与模块拆分无关 |
| `Src/World/AutoGen/*.gen.*` | Proto 类型注册位置不变 |
| `Tools/Script/GenMsgBindings.py` | 本次不修改（之后用 Phase E 处理） |

---

## 八、关键设计决策说明

### Q: 为什么 Timer 放在 Core 而不是 World？

Timer 只依赖 `TimingWheel`（`Common/Timer/TimingWheel.h`），不依赖 ECS 或 World。任何有定时器需求的服务（LoginServer 心跳、GateServer 超时、SocialServer 好友请求超时）都能复用。唯一隐藏依赖是回调中用到 `das::Context`——但 Context 已经是 Core 层通过 `DasCoreModule::SetContext()` 注入的。

### Q: 为什么 SendToClient 放在 Core？

`SendToClient` 只依赖 `IDasLangtHost::SendRawToClient()`。任意实现 `IDasLangtHost` 的服务器都能调用。但从语义上说，LoginServer 可能不需要向 Client 发消息——但这不影响它放在 Core 层（不做额外操作就没人调用它）。

### Q: 为什么 BattleStats 放在 Core？

`BattleStats` 是一个纯 POD struct，不依赖任何 ECS 组件。它的使用场景可能超出 WorldServer（如 SocialServer 的好友战斗属性比较、排行榜服务器）。放入 Core 保证类型在所有脚本环境中可访问。跨服务器的 proto 类型（`Vector3`、`ErrorInfo`）同理。

### Q: EntityGetBattleStats 为什么是 World？

`EntityGetBattleStats` 的实现需要 `ResolveEntity()` → `Scene::HasComponent<BattleStats>()` → `Scene::GetComponent<Health>()`。它依赖 ECS/EnTT——严格的 World 层。这与 BattleStats **类型定义**的归属是两个层面的问题。
