# 脚本接缝（ECS_06）

> 契约见 `ECS_00` §2（铁律 1/2）。本篇给出**可照抄实现**：
> `Bridge_*` extern 绑定 → 事件队列（C++ → 脚本）→ `[game_event]`/`[game_system]` 宏。
> 交付后：脚本能读/写组件（快照式）、响应游戏事件、跑低频决策系统。

---

## 1. 设计原则

1. **铁律 1**：脚本只拿 `uint64 EntityID` + 值拷贝（`float3`/`int32`/struct 快照），
   不持组件引用。写回经 `Bridge_EntitySetXxx` → 内部 `MarkDirty`。
2. **铁律 2**：脚本调用量 ∝ 事件数。事件队列（C++ → 脚本）每帧最多派发
   有限个事件；`[game_system]` 是**分频错峰**的低频决策（AI 1Hz、Buff 10Hz）。
3. **绑定层在 `DasCommonModule`**（服务无关）或 `WorldScriptModule`（World 专用）——
   依赖方向铁律：`ScriptEngine` 不依赖 World，World 的绑定放 `ProtoScriptModule`。

---

## 2. 绑定函数清单（`Bridge_*`）

### 2.1 通用（DasCommonModule——服务无关）

```cpp
// 已存在（保留）：
//   LogInfo/LogWarn/LogError（DasCommonModule.cpp）

// 新增（DasCommonModule 或独立 BridgeModule）：
void  Bridge_Log(const char *text, das::Context *, das::LineInfoArg *); // 已有 LogInfo

// 实体操作（依赖 Scene —— 但 DasCommonModule 不能依赖 World！
//    → 实体绑定放 WorldScriptModule（World 专用），见下）
```

> **关键约束**：`DasCommonModule` 是 `ScriptEngine` 的下层库，**不能 include World 的 Scene**。
> 所以 `Bridge_*` 实体绑定必须放 `WorldScriptModule`（`ProtoScriptModule` target），
> 通过 `IDasLangHost` 或模块提供者拿到场景。

### 2.2 World 专用（WorldScriptModule / ProtoScriptModule）

```cpp
// 实体查询
uint64 Bridge_EntityCreate(uint16 sceneId);                    // 创建实体（身份）
void   Bridge_EntityDestroy(uint64 entityID);                  // 销毁实体
bool   Bridge_EntityIsValid(uint64 entityID);                  // 有效性（version 校验）
float3 Bridge_EntityGetPosition(uint64 entityID);              // 位置（值拷贝）
void   Bridge_EntitySetPosition(uint64 entityID, float3 pos);  // 写位置（内部 MarkDirty）

// 属性
int32 Bridge_EntityGetHp(uint64 entityID);
void   Bridge_EntitySetHp(uint64 entityID, int32 hp);
int32 Bridge_EntityGetMaxHp(uint64 entityID);

// 标签查询
bool Bridge_EntityIsDead(uint64 entityID);
bool Bridge_EntityIsInCombat(uint64 entityID);
bool Bridge_EntityIsStunned(uint64 entityID);
bool Bridge_EntityIsPlayer(uint64 entityID);
bool Bridge_EntityIsMonster(uint64 entityID);

// 空间查询（依赖 Grid）
das::TArray<uint64> Bridge_EntitiesInRadius(float3 center, float radius, das::Context *ctx);
```

> **值类型**：`float3` 是 daslang 内建类型，C++ 侧是 `vec4f`（`__m128`）——绑定用
> `das::cast<float3>` 与 `__m128` 转换。参考既有 `BattleStats` 绑定（memory：`vec4f` 全局 typedef）。

---

## 3. 类型化事件（决策 3 定稿）

**决策 3**：每事件类型一个 struct，绑定代码由生成器自动产出。

### 3.1 事件定义（`Src/Proto/GameEvent.proto`）

```protobuf
// GameEvent.proto — 游戏事件定义（C++ 产出 → daslang [game_event] 消费）
syntax = "proto3";

package MMO.Proto;

// 事件类型枚举（uint16 值空间，与 das 侧 EGameEventType 一一对应）
enum EGameEventType
{
    GAME_EVENT_NONE              = 0;
    GAME_EVENT_ENTITY_ENTER_AOI  = 1;  // EntityEnterAOIEvent
    GAME_EVENT_ENTITY_LEAVE_AOI  = 2;  // EntityLeaveAOIEvent
    GAME_EVENT_ENTITY_DAMAGED    = 3;  // EntityDamagedEvent
    GAME_EVENT_ENTITY_DIED       = 4;  // EntityDiedEvent
    GAME_EVENT_ENTITY_SPAWNED    = 5;  // EntitySpawnedEvent
    GAME_EVENT_SKILL_CAST        = 6;  // SkillCastEvent
    GAME_EVENT_BUFF_APPLIED      = 7;  // BuffAppliedEvent
    GAME_EVENT_BUFF_EXPIRED      = 8;  // BuffExpiredEvent
}

message EntityEnterAOIEvent { uint64 observer_id = 1; uint64 entity_id = 2; }
message EntityLeaveAOIEvent { uint64 observer_id = 1; uint64 entity_id = 2; }
message EntityDamagedEvent  { uint64 target_id = 1; uint64 attacker_id = 2; int32 damage = 3; }
message EntityDiedEvent     { uint64 entity_id = 1; }
message EntitySpawnedEvent  { uint64 entity_id = 1; uint32 entity_type = 2; }
message SkillCastEvent      { uint64 caster_id = 1; uint64 target_id = 2; uint32 skill_id = 3; }
message BuffAppliedEvent    { uint64 target_id = 1; uint32 buff_id = 2; }
message BuffExpiredEvent    { uint64 target_id = 1; uint32 buff_id = 2; }
```

> **命名约定（唯一真相源，生成器 + das 宏共用）**：
> - 枚举 `GAME_EVENT_<TYPE>` ↔ 消息 `<Type>Event`（`GAME_EVENT_ENTITY_DAMAGED` ↔ `EntityDamagedEvent`）
> - 转换规则：驼峰 ↔ 下划线大写（与 `MsgNameToMsgID` 同规则）

### 3.2 `GameEventBus`（C++ 侧，服务无关）

```cpp
// Src/ScriptEngine/GameEventBus.h
#pragma once

#include <cstdint>
#include <vector>

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 游戏事件负载（序列化后的 protobuf 字节，值语义）
     *
     * data 指向 GameEventBus 内部稳定存储（帧内有效，派发后清空）。
     */
    struct GameEventEnvelope
    {
        uint16      type = 0;       // EGameEventType
        uint32      size = 0;       // payload 字节数
        const uint8 *data = nullptr; // payload（bus 内）
    };

    /**
     * @brief 每场景一个事件总线（C++ 写，脚本读）
     *
     * payload 存固定容量缓冲——零 per-event 堆分配（铁律 2 纪律）。
     */
    class GameEventBus
    {
    public:
        template <typename TEvent>
        void Emit(uint16 type, const TEvent &ev)
        {
            if (_envelopes.size() >= kMaxEventsPerFrame)
            {
                return; // 事件预算
            }
            const size_t sz = static_cast<size_t>(ev.ByteSizeLong());
            if (_payloadUsed + sz > _payload.size())
            {
                return;
            }
            uint8 *dst = _payload.data() + _payloadUsed;
            ev.SerializeToArray(dst, static_cast<int>(sz));
            _envelopes.push_back({type, static_cast<uint32>(sz), dst});
            _payloadUsed += sz;
        }

        std::vector<GameEventEnvelope> Drain()
        {
            std::vector<GameEventEnvelope> result;
            result.swap(_envelopes);
            _payloadUsed = 0;
            return result;
        }

        size_t Count() const { return _envelopes.size(); }

        static constexpr size_t kMaxEventsPerFrame = 1024;  // 铁律 2
        static constexpr size_t kMaxPayloadBytes   = 64 * 1024;

    private:
        std::vector<GameEventEnvelope> _envelopes;
        std::vector<uint8>             _payload{ kMaxPayloadBytes };
        size_t                         _payloadUsed = 0;
    };

} // namespace MMO
```

### 3.3 事件绑定生成器（`GenMsgBindings.py` 扩展）

在既有 `gen_msg_bindings` rule 基础上，新增"事件绑定"输出到
`Src/World/AutoGen/GameEventBindings.gen.{h,cpp}`：

```python
# GenMsgBindings.py 新增
def gen_game_event_bindings(proto_dir: str, out_dir: str) -> None:
    """
    扫描 GameEvent.proto，对每个 *Event message 生成：
      1. das::ManagedStructureAnnotation（事件类型绑定，含字段访问器）
      2. Emit<Type>Event(...) 工厂函数（C++ 侧调用点）
      3. RegisterAllGameEventBindings(mod, lib) 汇总
      4. EGameEventType 枚举绑定（das 侧可查值）
    """
```

生成的 `GameEventBindings.gen.cpp` 核心（每事件类型一个注解类）：

```cpp
// 自动生成——EntityDamagedEvent 绑定
namespace
{
    struct EntityDamagedEventAnnotation
        : das::ManagedStructureAnnotation<MMO::Proto::EntityDamagedEvent>
    {
        EntityDamagedEventAnnotation(das::ModuleLibrary &lib)
            : das::ManagedStructureAnnotation<MMO::Proto::EntityDamagedEvent>("EntityDamagedEvent", lib)
        {
            addProperty<DAS_BIND_MANAGED_PROP(target_id)>("target_id");
            addProperty<DAS_BIND_MANAGED_PROP(attacker_id)>("attacker_id");
            addProperty<DAS_BIND_MANAGED_PROP(damage)>("damage");
        }
    };

    // GameEventEnvelope 绑定（脚本只读 type，payload 由宏内部处理）
    struct GameEventEnvelopeAnnotation
        : das::ManagedStructureAnnotation<MMO::GameEventEnvelope>
    {
        GameEventEnvelopeAnnotation(das::ModuleLibrary &lib)
            : das::ManagedStructureAnnotation<MMO::GameEventEnvelope>("GameEventEnvelope", lib)
        {
            addProperty<DAS_BIND_MANAGED_PROP(type)>("type");
        }
    };
}

namespace MMO
{
    // 事件发射工厂（C++ 业务侧调用）
    void EmitEntityDamaged(uint64 target, uint64 attacker, int32 damage)
    {
        MMO::Proto::EntityDamagedEvent ev;
        ev.set_target_id(target);
        ev.set_attacker_id(attacker);
        ev.set_damage(damage);
        GameEventBus::Instance().Emit(MMO::Proto::GAME_EVENT_ENTITY_DAMAGED, ev);
    }
    // ... 每个事件类型一个 Emit<Type>

    void RegisterAllGameEventBindings(das::Module &mod, das::ModuleLibrary &lib)
    {
        mod.addAnnotation(new EntityDamagedEventAnnotation(lib));
        // ... 每个事件类型
    }
}
```

> **生成器改动点**（对照现有 `GenMsgBindings.py` 的消息绑定逻辑）：
> 1. 识别 `*Event` 消息（后缀约定）——现有逻辑识别 `*Req`，扩展识别规则
> 2. 生成 `ManagedStructureAnnotation`（复用现有消息绑定的注解生成逻辑）
> 3. 生成 `Emit<Type>` 工厂（新逻辑：C++ 调用点便捷函数）
> 4. `GameEventEnvelope` 绑定（手写一次，生成器包含模板）
> 5. 汇总 `RegisterAllGameEventBindings` 注册进 `ProtoBindIndex.gen.cpp`（或独立 index）

### 3.4 `[game_event]` 宏（类型化分发，das 侧）

```das
// Script/Common/GameEventRegistry.das
options gen2
options indenting = 4

module GameEventRegistry

require daslib.ast
require daslib.ast_boost
require daslib.macro_boost

require Common
require world    // WorldScriptModule：EGameEventType 枚举 + 各 *Event 类型 + GameEventEnvelope

// 事件分发表：type → handler(ev : void?)，宏注入类型化转换
var g_EventRegistry : table<uint16, function<(ev : void?) : void>>

// 事件名 → 枚举名（EntityDamagedEvent → GAME_EVENT_ENTITY_DAMAGED）
def private EventNameToEnum(name : string) : string
{
    // 1. 去 "Event" 后缀：EntityDamagedEvent → EntityDamaged
    // 2. 驼峰 → 下划线大写：EntityDamaged → ENTITY_DAMAGED
    // 3. 加前缀：→ GAME_EVENT_ENTITY_DAMAGED
    // 实现同 MsgNameToMsgID（复用字符串工具）
    ...
}

[function_macro(name="game_event")]
class GameEventHandlerAnnotation : AstFunctionAnnotation
{
    def override apply(var func : FunctionPtr, var group : ModuleGroup,
                       args : AnnotationArgumentList, var errors : das_string) : bool
    {
        // 签名: (ev : <Type>Event)——第一个参数类型即事件类型
        if (length(func.arguments) < 1)
        {
            errors := "[game_event] 需要至少一个参数: (ev : <事件类型>)"
            return false
        }

        // 从参数类型名推导事件枚举名
        let evType = func.arguments[0]._type
        if (evType.baseType != Type.tHandle || evType.annotation == null)
        {
            errors := "[game_event] 参数必须是事件类型（如 ev : EntityDamagedEvent）"
            return false
        }
        let evTypeName  = string(evType.annotation.name)   // "EntityDamagedEvent"
        let enumName    = EventNameToEnum(evTypeName)      // "GAME_EVENT_ENTITY_DAMAGED"

        // 查 world 模块的 EGameEventType 枚举
        let cmod = find_compiling_module("world")
        if (cmod == null) { errors := "未找到 world 模块——需 require world"; return false }
        let enu = module_find_enumeration(cmod, "EGameEventType")
        if (enu == null) { errors := "world 模块未绑定 EGameEventType"; return false }

        // typo 防护：确认枚举成员存在
        var found = false
        for (e in enu.list) { if (e.name == enumName) { found = true; break } }
        if (!found)
        {
            errors := "事件类型 {evTypeName} 推导出的 {enumName} 不在 EGameEventType 中"
            return false
        }
        let evID = uint16(find_enum_value(enu, enumName))

        // 注入注册：从 void? payload 反序列化出类型化事件再调用 handler
        var initBlk <- setup_call_list("game_event`init", func.at, true, true)
        initBlk.list |> push(qmacro_expr(${
            g_EventRegistry[$v(evID)] = @@(ev : void?) {
                let typed = unsafe(reinterpret<$t(evType)?> ev)
                $c("_::{func.name}")(*typed)
            }
        }))
        func.flags.privateFunction = true
        return true
    }
}

// C++ 每帧调用：事件总线 → daslang 数组 → 分发
[export]
def DispatchGameEvents(var envelopes : array<GameEventEnvelope>)
{
    for (e in envelopes)
    {
        g_EventRegistry |> get(e.type)$(handler)
        {
            // payload 指针作为 void? 传给 handler，宏内部 reinterpret
            let ptr = unsafe(reinterpret<void?> e.data)
            invoke(handler, ptr)
        }
    }
}
```

```cpp
// C++ 侧（WorldServer）：
void WorldServer::DispatchGameEventsToScript()
{
    auto *ctx = DasLangEngine::GetIns().GetScriptContext();
    auto *fn  = ctx->findFunction("DispatchGameEvents");
    if (!fn)
    {
        return;
    }

    // GameEventBus::Drain() → das::TArray<GameEventEnvelope>
    // 用 DasHelpers（已有）安全构造 TArray（参考 BattleStats 按值返回经验）
    auto events = _gameEventBus.Drain();
    // 构造 das::TArray<GameEventEnvelope> 并传入
    ...
    ctx->evalWithCatch(fn, args);
}
```

> **类型安全**：`[game_event]` 宏在**编译期**从参数类型推导事件枚举 ID——handler 签名
> 与事件类型绑定，写错类型编译即报错（typo 防护）。运行期 `reinterpret` 是安全的：
> payload 是生成器序列化的事件 struct，与 handler 参数类型**由同一枚举 ID 关联**。

---

## 4. （原定长事件版本已废弃——见 ECS_09 决策 3）

> 原 `GameEvent` 定长 POD 方案（`(type,a,b,c)`）被决策 3 取代：类型化事件 + 生成器绑定。
> 保留本节的语义：事件预算（`kMaxEventsPerFrame`）、每帧派发清空、`[game_system]` 分频错峰。

---

## 5. `[game_system]` 宏（低频决策）

```das
// Script/Common/GameSystemRegistry.das
options gen2
options indenting = 4

module GameSystemRegistry

require daslib.ast
require daslib.ast_boost
require daslib.macro_boost

require Common

// 系统注册表：name → (dt)
var g_SystemRegistry : table<string, function<(dt : float) : void>>

// 分频错峰：每个系统注册自己的调用间隔（秒）
struct SystemSpec
{
    interval : float  // 调用间隔（0 = 每帧）
    lastTime : float  // 上次调用时间
}

var g_SystemSpecs : table<string, SystemSpec>

[function_macro(name="game_system")]
class GameSystemAnnotation : AstFunctionAnnotation
{
    def override apply(var func : FunctionPtr, var group : ModuleGroup,
                       args : AnnotationArgumentList, var errors : das_string) : bool
    {
        // 参数: [game_system(interval = 1.0)] — 1 秒调一次
        let interval = 0.0
        let arg = find_arg(args, "interval")
        if (arg is tFloat)
        {
            interval = arg as tFloat
        }

        let name = string(func.name)
        var initBlk <- setup_call_list("game_system`init", func.at, true, true)
        initBlk.list |> push(qmacro_expr(${
            g_SystemRegistry[$v(name)] = @@(dt : float) {
                $c("_::{func.name}")(dt)
            }
            g_SystemSpecs[$v(name)] = SystemSpec(interval = $v(interval), lastTime = 0.0)
        }))
        func.flags.privateFunction = true
        return true
    }
}

// C++ 每帧调用：错峰调度
[export]
def TickGameSystems(dt : float; now : float)
{
    for (name, spec in g_SystemSpecs)
    {
        if (spec.interval <= 0.0 || now - spec.lastTime >= spec.interval)
        {
            g_SystemRegistry |> get(name)$(sys)
            {
                invoke(sys, dt)
            }
            spec.lastTime = now
        }
    }
}
```

```cpp
// C++ 侧（WorldServer 每帧）：
void WorldServer::TickGameSystems(float dt)
{
    auto *ctx = DasLangEngine::GetIns().GetScriptContext();
    auto *fn  = ctx->findFunction("TickGameSystems");
    if (!fn)
    {
        return;
    }
    // now = 累计运行时间（秒）
    vec4f args[] = {
        das::cast<float>::from(dt),
        das::cast<float>::from(_accumTime),
    };
    _accumTime += dt;
    ctx->evalWithCatch(fn, args);
}
```

---

## 6. 绑定实现细节

### 6.1 `Bridge_EntityGetPosition`（值拷贝）

```cpp
// WorldScriptModule.cpp 内注册
vec4f Bridge_EntityGetPosition(uint64 entityID, das::Context *ctx, das::LineInfoArg *at)
{
    // 经 IDasLangHost / 模块提供者拿场景
    auto *host = ...; // WorldScriptModule 持有 WorldServer 指针（构造注入）
    auto *scene = host->GetSceneByEntityID(entityID);
    if (!scene)
    {
        return das::cast<float3>::from(float3(0, 0, 0));
    }
    auto e = scene->Resolve(entityID);
    if (e == entt::null || !scene->Registry().all_of<Position>(e))
    {
        return das::cast<float3>::from(float3(0, 0, 0));
    }
    auto &pos = scene->Registry().get<Position>(e);
    return das::cast<float3>::from(float3(pos.x, pos.y, pos.z));
}
```

> `float3` 的 C++ 对应：`das::float3`（daScript 有 `float3` 内建类型，C++ 侧是
> `das::float3` 结构或 `vec4f` 别名）。绑定用 `das::cast<float3>::from(...)` 返回 `vec4f`。

### 6.2 场景访问（模块提供者模式）

```cpp
// IDasLangHost 扩展：提供场景查询（WorldServer 实现）
class IDasLangHost
{
    virtual ECS::Scene *GetSceneByEntityID(uint64 entityID) = 0;
    virtual ECS::Scene *GetDefaultScene() = 0;
    virtual GameEventBus &GetGameEventBus() = 0;  // 事件总线（WorldServer 持有）
};
```

> **事件发射**：业务侧调 `GameEventBus::Emit<TypeEvent>(ev)`（生成器提供的 `Emit<Type>` 工厂
> 包装），不再走宿主接口——宿主只暴露 `GetGameEventBus()`。

---

## 7. 构建脚本变动

- `Src/Proto/GameEvent.proto`：**新建**——事件定义（proto_gen rule 自动生成 `GameEvent.pb.h/.cc`）
- `Src/ScriptEngine/xmake.lua`：新增 `GameEventBus.h`（header-only）——无需改
- `Src/World/DasModule/xmake.lua`：
  - `WorldScriptModule.cpp` 扩展 `Bridge_*` 注册——文件内直接加
  - `gen_msg_bindings` rule 扩展：生成 `GameEventBindings.gen.{h,cpp}`（决策 3 生成器）
  - `ProtoBindIndex.gen.*` 汇总 `RegisterAllGameEventBindings`
- `Script/Common/`：新增 `GameEventRegistry.das`/`GameSystemRegistry.das`——daslib 扫描自动纳入
- `Script/World/main.das`：`require GameEventRegistry` + `require GameSystemRegistry`

---

## 8. 验证步骤（本篇验收）

```powershell
# 1. 构建
xmake build WorldServer

# 2. 脚本冒烟：
#    - main.das 加一个 [game_system(interval=1.0)] def ai_tick(dt) { LogInfo("ai tick") }
#    - 启动后每 1 秒日志出现 "ai tick"
#    - C++ 侧 EmitEntityDamaged(targetID, attackerID, 100)（生成器工厂）
#      → 脚本 [game_event] def EntityDamaged(ev : EntityDamagedEvent) 收到并 LogInfo
```

**验收标准**：
- [ ] 构建零错误
- [ ] `GameEvent.pb.h` + `GameEventBindings.gen.*` 生成成功
- [ ] `Bridge_EntityGetPosition` 返回正确位置
- [ ] `[game_system]` 错峰调度正确（interval 生效）
- [ ] `[game_event]` 类型化分发正确（`ev : EntityDamagedEvent` 收到 `ev.damage`）
- [ ] 事件类型写错编译报错（typo 防护生效）
- [ ] 热重载后注册表重建（`[init]` 重跑），事件/系统继续工作

---

## 9. 踩坑预警

1. **绑定层位置**：`Bridge_*` 实体绑定必须放 `WorldScriptModule`（依赖 World 的 Scene），
   **不能**放 `DasCommonModule`（依赖方向铁律）。`DasCommonModule` 只留通用绑定（Log/时间）。
2. **`float3` C++ 表示**：`das::cast<float3>::from()` 需要 daScript 的 float3 类型——
   头文件 `daScript/simulate/simulate.h` + vecmath。参考 memory 中 `vec4f` 全局 typedef 经验。
3. **事件参数类型**：`GameEvent` 定长 `(a,b,c)` 是 POD——脚本数组 `array<GameEvent>` 需要
   `ManagedStructureAnnotation` 注册。或更简单：事件队列用 `array<uint64>` + type 分开传。
4. **注册表重建**：热重载 `DoSwap` 换 context 后，`[init]` 重跑填充 `g_EventRegistry`——
   事件/系统注册表是脚本侧状态，随 context 销毁重建，**无数据丢失**（铁律 1 推论）。
5. **`TickGameSystems` 的 `now`**：用引擎累计时间而非 wall clock——保证确定性与热重载一致性。
