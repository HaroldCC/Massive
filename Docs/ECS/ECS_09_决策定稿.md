# 三个实施决策点定稿（ECS_09）

> 本文档固化用户拍板的 3 个实施决策，并给出对应的设计调整。
> **权威**：后续实施以本文档为准，替代 ECS_00 §6 中的相关暂定项。

---

## 决策 1：场景粒度 = 方案 C（串行模拟 + 复制并行），先 A 起步

### 定稿内容

| 项 | 定稿 |
|---|---|
| 模拟阶段（PreUpdate→EventDispatch） | 单线程串行（LogicThread 一个循环 tick 所有 World） |
| 复制阶段（Replicate） | 线程池并行（每玩家一个打包任务） |
| 演进路径 | 先串行可编译 → 复制打包解耦成 per-player 任务 → 接入线程池 |

### 设计调整（相对 ECS_05）

1. **`ReplicateSystem` 拆成"打包函数（纯函数）+ 调度器"**：
   - `PackPlayerNtf(reg, observerIdx, visibleSet) -> Proto::EntityReplicateNtf` —— 纯只读，无副作用
   - `ReplicateScheduler::Update(...)` —— 负责遍历玩家、把打包任务投递到线程池、收集结果、发送
2. **线程池**：复用 `IOContextPool` 的 worker 或新建专用池。**关键**：打包是 CPU 密集（protobuf 序列化），与 IO 线程（网络收发）混用会互相拖累——建议**专用复制池**（`Common/ThreadPool`，若项目没有则新建，或用 `std::async` + 固定线程数）。
3. **只读约束**：并行打包期间 registry **必须无写**——模拟已完成、下一帧未开始。用"模拟与复制之间显式 barrier"保证（单线程模拟结束 → 发布只读快照标记 → 并行打包 → join → 下一帧）。

### 对 ECS_03 的调整

`World::Tick(dt)` 的 8 阶段中，`Replicate` 阶段从"同步调用"改为"发布只读快照 + 投递并行任务"：

```cpp
// World.cpp
void World::Tick(float dt)
{
    _scheduler.RunStage(EStage::PreUpdate, dt);
    _scheduler.RunStage(EStage::ScriptLogic, dt);
    _scheduler.RunStage(EStage::Movement, dt);
    _scheduler.RunStage(EStage::SpatialIndex, dt);
    _scheduler.RunStage(EStage::AOI, dt);
    _scheduler.RunStage(EStage::EventDispatch, dt);
    _scheduler.RunStage(EStage::PostUpdate, dt);

    // Replicate：发布快照 + 投递（不阻塞模拟线程）
    _replicateDispatcher.DispatchFrame(); // 内部：快照 + 池化打包 + 发送
    _replicateDispatcher.JoinFrame();      // 可选：需要复制完成才能改 registry 时 join
}
```

> **注意**：MVP 先同步（`JoinFrame` 立即 join），并行是 `DispatchFrame` 返回后异步跑。
> 实现顺序：先同步正确 → 再异步提速（两者 `DispatchFrame` 签名一致，切换成本低）。

---

## 决策 2：协议直接改字段类型（uint32 → uint64）

### 定稿内容

当前开发阶段，无旧客户端——**直接改** `Replicate.proto`：

```protobuf
message EntitySpawnNtf
{
    uint64 entity_id   = 1;  // uint32 → uint64（EntityID 契约）
    uint32 entity_type = 2;
    PositionDelta position = 3;
    int32  hp_current = 4;
    int32  hp_max     = 5;
}

message EntityUpdateNtf
{
    uint64 entity_id = 1;
    optional PositionDelta position    = 2;
    optional int32         hp_current  = 3;
    optional bool          is_dead     = 4;
    optional bool          is_in_combat = 5;
}

message EntityDespawnNtf
{
    uint64 entity_id = 1;
}
```

### 连带改动

1. **`Login.proto`**：`LoginEnterWorldRsp.player_id` 直接改 `uint64`（同一契约）：

```protobuf
message LoginEnterWorldRsp
{
    uint32 error_code = 1;   // 既有（若存在）
    uint64 player_id  = 2;   // uint32 → uint64
    uint32 scene_id   = 3;
    ...
}
```

> 需先读 `Login.proto` 确认字段现状再改。

2. **`EnterWorldHandler.cpp`**：`rsp.set_player_id(entityID)`（uint64 直接传，不再截断）。
3. **`TestClient`**：读 `rsp.player_id()` 处类型自动适配（uint64）。
4. **生成器**：`GenMsgBindings.py` 的 `_CPP_SCALAR_MAP` 已含 `uint64 → uint64_t`——无需改。
   `*.gen.cpp` 里 `das::cast<uint32_t>` 需改为 `das::cast<uint64_t>`（若 MoveReq/Login 的
   分发函数用了 uint32 字段）——生成器对字段类型是**从 proto 推导**的，改 proto 自动同步。

### 风险

- **`entity_id` 是 uint64，EnTT `to_integral` 是 uint32**——复制时用
  `Scene::ToEntityID(e)` 拿完整 uint64（ECS_05 已警示，现在是硬要求）。

---

## 决策 3：类型化事件 + 自动绑定代码生成

### 定稿内容

每事件类型一个 struct，**绑定代码由生成器自动产出**（不手写 `ManagedStructureAnnotation`）。

### 3.1 事件定义（新 `.proto` 或现有扩展）

新建 `Src/Proto/GameEvent.proto`：

```protobuf
// GameEvent.proto — 游戏事件定义（C++ 产出 → daslang [game_event] 消费）
syntax = "proto3";

package MMO.Proto;

// 事件类型枚举（uint16 值空间，与 das 侧 EGameEventType 一一对应）
enum EGameEventType
{
    GAME_EVENT_NONE           = 0;
    GAME_EVENT_ENTITY_ENTER_AOI = 1;  // EntityEnterAOIEvent
    GAME_EVENT_ENTITY_LEAVE_AOI = 2;  // EntityLeaveAOIEvent
    GAME_EVENT_ENTITY_DAMAGED   = 3;  // EntityDamagedEvent
    GAME_EVENT_ENTITY_DIED      = 4;  // EntityDiedEvent
    GAME_EVENT_ENTITY_SPAWNED   = 5;  // EntitySpawnedEvent
    GAME_EVENT_SKILL_CAST       = 6;  // SkillCastEvent
    GAME_EVENT_BUFF_APPLIED     = 7;  // BuffAppliedEvent
    GAME_EVENT_BUFF_EXPIRED     = 8;  // BuffExpiredEvent
}

// 每个事件一个 message（字段即事件负载）
message EntityEnterAOIEvent { uint64 observer_id = 1; uint64 entity_id = 2; }
message EntityLeaveAOIEvent { uint64 observer_id = 1; uint64 entity_id = 2; }
message EntityDamagedEvent  { uint64 target_id = 1; uint64 attacker_id = 2; int32 damage = 3; }
message EntityDiedEvent     { uint64 entity_id = 1; }
message EntitySpawnedEvent  { uint64 entity_id = 1; uint32 entity_type = 2; }
message SkillCastEvent      { uint64 caster_id = 1; uint64 target_id = 2; uint32 skill_id = 3; }
message BuffAppliedEvent    { uint64 target_id = 1; uint32 buff_id = 2; }
message BuffExpiredEvent    { uint64 target_id = 1; uint32 buff_id = 2; }
```

> **命名约定（唯一真相源）**：
> - 枚举名 `GAME_EVENT_<TYPE>` ↔ 消息名 `<Type>Event`
> - `GAME_EVENT_ENTITY_ENTER_AOI` ↔ `EntityEnterAOIEvent`（驼峰↔下划线大写）
> - 生成器 + das 宏都按此约定推导，不手写查表

### 3.2 生成器扩展（`GenMsgBindings.py`）

在既有 `gen_msg_bindings` rule 基础上，新增"事件绑定"输出。生成到
`Src/World/AutoGen/GameEventBindings.gen.{h,cpp}`：

```python
# GenMsgBindings.py 新增函数
def gen_game_event_bindings(proto_dir: str, out_dir: str) -> None:
    """
    扫描 GameEvent.proto：
    1. 枚举 EGameEventType → 值常量（das 侧 EGameEventType 绑定）
    2. 每个 *Event message → 生成：
       a. C++: ManagedStructureAnnotation（类型绑定到 das）
       b. C++: struct GameEventEnvelope { uint16 type; std::vector<uint8> payload; }
          —— 事件队列负载 = 序列化的 event message
       c. C++: MakeGameEvent<T>(args...) 工厂（emplace + 序列化）
       d. das 侧: [game_event] 宏自动推导（见 §3.3）
    3. 汇总 RegisterAllGameEventBindings(mod, lib)
    """
```

生成的 `GameEventBindings.gen.cpp` 核心（每事件类型）：

```cpp
// 事件类型绑定（ManagedStructureAnnotation——自动生成）
struct EntityDamagedEventAnnotation : das::ManagedStructureAnnotation<MMO::Proto::EntityDamagedEvent>
{
    EntityDamagedEventAnnotation(das::ModuleLibrary &lib)
        : das::ManagedStructureAnnotation<MMO::Proto::EntityDamagedEvent>("EntityDamagedEvent", lib)
    {
        addProperty<DAS_BIND_MANAGED_PROP(target_id)>("target_id");
        addProperty<DAS_BIND_MANAGED_PROP(attacker_id)>("attacker_id");
        addProperty<DAS_BIND_MANAGED_PROP(damage)>("damage");
    }
};

// 事件工厂——C++ 侧产生事件
void EmitEntityDamaged(uint64 target, uint64 attacker, int32 damage)
{
    MMO::Proto::EntityDamagedEvent ev;
    ev.set_target_id(target);
    ev.set_attacker_id(attacker);
    ev.set_damage(damage);
    GameEventBus::Emit(MMO::Proto::GAME_EVENT_ENTITY_DAMAGED, ev);
}
```

### 3.3 das 侧 `[game_event]` 宏（类型化分发）

```das
// Script/Common/GameEventRegistry.das
options gen2
options indenting = 4

module GameEventRegistry

require daslib.ast
require daslib.ast_boost
require daslib.macro_boost

require Common
require world    // WorldScriptModule：绑定 EGameEventType 枚举 + 各 *Event 类型

// 事件分发表：type → handler(ev : void?)，宏注入类型化转换
var g_EventRegistry : table<uint16, function<(ev : void?) : void>>

// 事件名 → 枚举名（EntityDamaged → GAME_EVENT_ENTITY_DAMAGED）
def private EventNameToEnum(name : string) : string
{
    // 同 MsgNameToMsgID（驼峰 → 下划线大写 + GAME_EVENT_ 前缀）
    ...
}

[function_macro(name="game_event")]
class GameEventHandlerAnnotation : AstFunctionAnnotation
{
    def override apply(var func : FunctionPtr, var group : ModuleGroup,
                       args : AnnotationArgumentList, var errors : das_string) : bool
    {
        // 签名: (ev : <Type>Event)——第二个参数是事件类型
        if (length(func.arguments) < 1) { errors := "需要至少一个参数"; return false }

        // 从参数类型取事件名（EntityDamagedEvent → EntityDamaged）
        let evType = func.arguments[0]._type
        // 校验是 handled type（消息类型绑定）
        ...
        let evName = string(evType.annotation.name)          // "EntityDamagedEvent"
        let evShort = 去掉 "Event" 后缀                         // "EntityDamaged"
        let enumName = EventNameToEnum(evShort)               // "GAME_EVENT_ENTITY_DAMAGED"

        // 查枚举值
        let cmod = find_compiling_module("world")
        let enu = module_find_enumeration(cmod, "EGameEventType")
        let evID = uint16(find_enum_value(enu, enumName))

        // 注入注册代码：从 void? 反序列化出类型化事件再调用 handler
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

// C++ 每帧调用：事件队列 → daslang
[export]
def DispatchGameEvents(var envelopes : array<GameEventEnvelope>)
{
    for (e in envelopes)
    {
        g_EventRegistry |> get(e.type)$(handler)
        {
            invoke(handler, e.payloadPtr)  // 指向序列化后的事件数据
        }
    }
}
```

> **关键设计**：事件负载是**序列化字节**（`GameEventEnvelope{type, payloadPtr, payloadLen}`），
> das 侧 `[game_event]` 宏按类型 `reinterpret` 成对应 struct 再调用 handler——类型安全由
> 宏编译期推导（参数类型 → 事件类型 → 枚举 ID），运行期零分配（payload 是字节数组的
> 只读视图）。`GameEventEnvelope` 是 C++ 绑定到 das 的句柄类型，含 `payloadPtr` 指针。

### 3.4 事件总线（C++ 侧）

```cpp
// Src/ScriptEngine/GameEventBus.h（服务无关）
#pragma once

#include <cstdint>
#include <vector>

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 游戏事件负载（序列化后的 protobuf 字节，值语义）
     */
    struct GameEventEnvelope
    {
        uint16              type  = 0;        // EGameEventType
        uint32              size  = 0;        // payload 字节数
        const uint8        *data  = nullptr;  // 指向 payload（事件总线内稳定存储）
    };

    /**
     * @brief 每场景一个事件总线（C++ 写，脚本读）
     *
     * payload 存固定容量环形缓冲——事件在帧内有效，派发后整体清空。
     * 避免 per-event 堆分配。
     */
    class GameEventBus
    {
    public:
        template <typename TEvent>
        void Emit(uint16 type, const TEvent &ev)
        {
            if (_envelopes.size() >= kMaxEventsPerFrame)
            {
                return; // 铁律 2：事件预算
            }
            // 序列化进 payload 缓冲
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

        static constexpr size_t kMaxEventsPerFrame = 1024;
        static constexpr size_t kMaxPayloadBytes   = 64 * 1024;

    private:
        std::vector<GameEventEnvelope> _envelopes;
        std::vector<uint8>             _payload{ kMaxPayloadBytes };
        size_t                         _payloadUsed = 0;
    };

} // namespace MMO
```

### 3.5 `GameEventEnvelope` 绑定到 das

```cpp
// GameEventBindings.gen.cpp（自动生成）
// GameEventEnvelope 绑定为 handled type + 字段访问器
struct GameEventEnvelopeAnnotation : das::ManagedStructureAnnotation<MMO::GameEventEnvelope>
{
    GameEventEnvelopeAnnotation(das::ModuleLibrary &lib)
        : das::ManagedStructureAnnotation<MMO::GameEventEnvelope>("GameEventEnvelope", lib)
    {
        addProperty<DAS_BIND_MANAGED_PROP(type)>("type");
        // size/data 不暴露给脚本——脚本只用 type 查分发表，payload 由宏内部处理
    }
};

// das 侧数组：array<GameEventEnvelope> 由 ctx 传入 DispatchGameEvents
```

> **说明**：脚本侧不需要访问 payload 字节——`[game_event]` 宏内部 `reinterpret`。
> 所以绑定只需暴露 `type` 字段。数组 `array<GameEventEnvelope>` 由 C++ 构造后
> 传给 `DispatchGameEvents`（用 `das::TArray`，参考既有 BattleStats 按值返回经验）。

---

## 附：三决策对文档的修订点

| 文档 | 修订 |
|---|---|
| ECS_00 §4.4 | Replicate 阶段注明"并行打包（方案 C）" |
| ECS_01 §6 | `rsp.set_player_id(entityID)` 直接 uint64 |
| ECS_05 §2/§9 | `entity_id` 直接 uint64（无兼容字段）；`Scene::ToEntityID` 是硬要求 |
| ECS_05 §3 | `ReplicateSystem` 拆"打包纯函数 + 调度器" |
| ECS_06 全文 | 事件改类型化：`GameEvent.proto` + 生成器 + `GameEventBus` + `[game_event]` 宏类型化分发 |
| ECS_08 §1.6 | `Replicate.proto` 直接改类型（非加字段）；新增 `GameEvent.proto` + `GameEventBindings.gen.*` |

---

## 补充决策 4：强类型 EntityID（2026-08-03）

**背景**：用户要求实体 ID 明确类型化，不用裸 `uint32`/`uint64`（类比 EnTT 的
`enum class entity`）。

**定稿**：`EntityID` 用 **struct 包装**而非 enum class：

```cpp
struct EntityID
{
    uint64 raw = 0;
    constexpr EntityID() = default;
    constexpr EntityID(uint64 value) : raw(value) {}
    constexpr bool operator==(const EntityID &) const = default;
    constexpr bool operator!=(const EntityID &) const = default;
    constexpr explicit operator bool() const { return raw != 0; }
    constexpr operator uint64() const { return raw; }
    static constexpr EntityID Invalid() { return EntityID(0); }
};
// + std::hash / std::formatter 特化
```

**为什么 struct 而非 enum class**：

| 维度 | struct 包装 | enum class |
|---|---|---|
| 位运算（MakeEntityID/SceneOf/...） | 封装在函数内，调用方无感 | 必须到处 `static_cast` |
| 隐式转 uint64（跨语言/协议） | `operator uint64()` 零摩擦 | 必须 cast |
| 容器键/日志 | `std::hash`/`std::formatter` 特化 | 同（也需特化） |
| 类型安全 | ✅ 与整数强区分 | ✅ 更强（连隐式转换都禁） |
| 可扩展（位段访问） | ✅ 可加方法 | ❌ 只能自由函数 |

**跨边界规则**（实施强制）：
1. C++ 内部：一律 `ECS::EntityID`，禁止裸整数混用
2. 网络/DB/脚本：`static_cast<uint64>(eid)`（隐式）进出；protobuf 字段 `uint64`
3. 日志：`Log::Info("{}", eid)`（formatter 特化）
4. 容器：`unordered_map<EntityID, T>`（hash 特化）

**代码已落地**：`Src/Common/ECS/EntityID.h`（强类型 struct + hash/formatter 特化）、
`EntityRegistry::Create()` 返回 `EntityID`、`WorldSession::entityID`/`AIState::targetID`
均为 `EntityID`、`EnterWorldHandler` 用 `EntityID` 且 `rsp.set_player_id(static_cast<uint64>)`。
构建验证通过。

---

## 补充决策 5：EntityIndex（2026-08-04）

**背景**：用户指出 `Grid::entityIndex` 参数（裸 `uint32`）是 EnTT 内部索引而非 EntityID，
语义混淆。

**定稿**：新增 `struct EntityIndex { uint32 raw; }` 强类型（同 EntityID 模式），
用于**对内热路径索引**（Grid 格子 / DirtyIndex 脏标记 / SystemAOI 候选集 / 复制打包）。

**EntityID vs EntityIndex 边界**（实施强制）：
- 对外（session/脚本/网络/组件 API）：`EntityID`（64 位，scene|index|version）
- 对内热路径（Grid/DirtyIndex/AOI/复制）：`EntityIndex`（32 位，EnTT index）
- 转换：`EntityID → EntityIndex` 用 `IndexOf(eid)`；`EntityIndex → entt::entity` 用
  `entt::entity(static_cast<entt::id_type>(idx.raw))`

**代码已落地**：`EntityID.h` 新增 `EntityIndex` + hash/formatter 特化；
`Grid`/`DirtyIndex`/`SystemAOI` 统一用 `EntityIndex`。构建验证通过。
