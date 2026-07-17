# 基础设计 #9：脚本引擎 · DasLang + DECS 集成

> 状态：**设计中**（2026-07-15 修订——修正了代码库同步导致的偏差，补齐了 4 个设计缺口）
> 关联：[s13_ecs_core](../前期设计/ecs/s13_ecs_core.html)（ECS 架构）、[s14_script_api](../前期设计/ecs/s14_script_api.html)（脚本 API 原案）、[ai_system](../前期设计/ecs/ai_system.html)（AI 框架）、[01_Network](01_Network.md)（TCP 连接层）、[03_RPC](03_RPC.md)（内部 RPC）
> 决定大幅偏离 s14 原案的理由见 §2。

## 0. 项目现状与前提

### 0.1 已具备的基础设施

| 设施 | 位置 | 状态 |
|------|------|------|
| daScript 运行时 | `ThirdParty/daScript/` + `ThirdParty/daslang.lua` | ✅ 已集成，三级 target 构建（libUriParser → libDaScript_runtime → libDaScript），MSVC 兼容（`/bigobj` `/EHa`） |
| daslib 标准库（含 DECS） | `ThirdParty/daScript/daslib/`（`decs.das`, `decs_boost.das`, `decs_state.das`） | ✅ 已存在——纯 .das 脚本，运行时 require，无需 C++ 编译 |
| EnTT | `ThirdParty/entt/` | ✅ 已集成（header-only），`CommonECS` target 已依赖 |
| Scene | `Src/Common/ECS/Scene.h/.cpp` | ✅ 已实现——EnTT registry + ScriptComponentStorage 双存储 |
| Entity | `Src/Common/ECS/Entity.h` | ✅ 已实现——(sceneID, entityID) 对，`kInvalidID = 0xFFFFFFFF` 哨兵 |
| ScriptComponentStorage | `Src/Common/ECS/ScriptComponentStorage.h/.cpp` | ✅ 已实现——SoA Blob 列，swap-with-last O(1) 删除 |
| DirtyTracker | `Src/Common/ECS/DirtyTracker.h` | ✅ 已实现——按组件类型脏标记 |
| LogicThread | `Src/World/LogicThread.h/.cpp` | ✅ 已实现——20ms 固定 Tick，消息预算自适应，过载保护 |
| MessageDispatcher | `Src/Common/Network/MessageDispatcher.h` | ✅ 已实现——O(1) 查表分派，模板自动反序列化 Protobuf |
| TimingWheel | `Src/Common/Timer/` | ✅ 已实现——三级时间轮定时器 |
| Tracy 埋点 | `MASSIVE_PROFILE` 宏 | ✅ 已集成 |

> **注意**：上述 EnTT/Scene/Entity 等所有文件均位于 `namespace MMO::ECS` 或 `namespace MMO` 下。命名空间这一事实在所有代码示例中的类型引用（如 `ECS::Scene`、`MMO::Entity`）中已隐含。

### 0.2 尚未构建的内容（引入脚本引擎的最佳时机）

| 缺失项 | 说明 |
|--------|------|
| C++ Component 定义 | Position/Velocity/Health/BattleStats 等都在设计文档中，未写入代码 |
| ECS System | `Src/World/System/` 目录存在但为空——Movement/Combat/AOI/Replicate 全部未实现 |
| Handler（C++ 业务） | 仅 EnterWorld + Move 两个 Handler，且 MoveHandler 已内联为 lambda——无历史包袱 |
| 脚本文件 | 无一行的 `.das` 文件 |
| WorldServer xmake 依赖 | `Src/World/xmake.lua` 未 add_deps("libDaScript") |
| WorldServer 脚本成员 | `WorldServer.h` 无 `das::Context` / DasLang 相关成员

**结论**：当前是引入脚本引擎的最佳时间窗口。不存在"C++ Handler 堆积完再迁到脚本"的痛苦——可以直接按本文设计从 Day 1 构建脚本层。

### 0.3 前置条件：DECS 与 daslib 状态

DECS 位于上游 daScript 仓库的 `ThirdParty/daScript/daslib/` 中（`decs.das`, `decs_boost.das`, `decs_state.das`），已在 repo 内，**不需要额外 submodule**。

DECS 是纯 DasLang 脚本模块：脚本侧通过 `require daslib/decs_boost` 在运行时加载，不参与 C++ xmake 编译链。核心运行时 `libDaScript` 通过 `daslang.lua` 已完整编译。

```
ThirdParty/daScript/
├── daslib/                     ✅ 已存在——标准库脚本
│   ├── decs.das                ← DECS ECS 核心（archetype/component/query）
│   ├── decs_boost.das          ← DECS 增强（query with/without 宏、Stage 调度）
│   ├── decs_state.das          ← DAP debugger 可视化
│   ├── archive.das             ← 序列化引擎
│   └── ...其他 lib（json/coroutines/ast 等）
└── include/                    ✅ C++ 头文件（das::Context 等）
```

**Phase 1 无需安装新依赖。** `WorldServer` 只需 `add_deps("libDaScript")` 即可开始脚本集成。

---

## 1. 定位

脚本层负责所有**可热更新的游戏业务逻辑**——Buff 结算、技能 CD、AI 决策、任务进度、掉落判定等。物理模拟（Position/Velocity 更新、碰撞检测、AOI 空间索引）保留在 C++ EnTT 中，脚本只读不写。

| 属性 | 值 |
|------|-----|
| 脚本语言 | DasLang（gen2 syntax） |
| ECS 运行时 | DECS（daslib/decs + decs_boost）— DaScript 原生 archetypal ECS |
| 热更新 | `Context::restart()` + `relocateCode()`，状态在 DECS + EnTT 中 |
| 与 C++ 的桥接 | ~18 个窄函数（编译期绑定，零字符串），非通用 ECS 查询系统 |
| 线程模型 | **LogicThread 独占调用**——所有脚本函数在 C++ LogicThread 中同步执行 |

## 2. 为什么大幅偏离 s14 原案

原案（`s14_script_api.html`）设计了一套自定义 ECS API（`ecs_query` / `ecs_edit` / `ecs_stage`），目标是让脚本通过宏操作 EnTT 中的 Component。经过深入评估，发现三个无法逾越的断层：

### 断层 1：ecs_query 需要 C++ 编译期模板实例化

原案的 `ecs_query(sceneId; pos: Position&; vel: Velocity)` 设想编译期展开为：

```cpp
auto group = registry.group<entt::owned_t<Position, Velocity>>();
// → owning group 保证 raw() 对齐 → DasLang temp array 零拷贝
```

但 DasLang 宏生成的是 DasLang AST 节点，**不能驱动 C++ 模板实例化**。每出现一个新 Component 组合（`Position+Health`、`Position+Velocity+Mana`...），C++ 端都需要预编译一个对应的桥接函数。N 个组件 = 2^N 种组合，不可行。

### 断层 2：EnTT 无组件内省机制

原案的 `ecs_edit` 依赖"遍历 Scene 注册的所有 EnTT 组件 → 自动填充 ComponentMap"：

```cpp
for (auto& storage : scene.EcsRegistry().storage()) {
    if (storage.contains(e))
        cmp.enTTFields[typeName] = &storage.get(e);
}
```

EnTT 的 `storage()` 返回类型擦除的 `sparse_set*`——**不通过 `static_cast` 回具体类型就无法取出 `T&`**。需要一个独立的组件元数据注册表来记住每个名字对应的 getter/setter。这不是不可行，但额外增加了大量基础设施。

### 断层 3：DasLang 生态已有 DECS ——不需要从零造

DECS 是 DaScript 官方 ECS 模块，功能完整：

| 能力 | DECS 覆盖 |
|------|-----------|
| 动态组件（运行时决定实体有哪些组件） | ✅ `create_entity()` + `cmp.name := value` |
| 声明式查询（archetype 级 SoA 批量遍历） | ✅ `query() $(pos: float3&; hp: int) { ... }` |
| Stage 调度（游戏循环的阶段组织） | ✅ `[decs(stage = "combat")]` + `decs_stage("combat")` |
| 类型安全的 struct 模板 | ✅ `[decs_template] struct Particle { ... }` |
| 批量创建（~7× 性能提升） | ✅ `create_entities\`Particle(1000) $(...) { ... }` |
| 序列化/反序列化（跨热更新持久化） | ✅ `mem_archive_save` / `mem_archive_load` |
| 热更新状态保留 | ✅ `decs_live` 一行 require，自动 save/restore |
| 调试支持 | ✅ `decs_state` — DAP debugger 中可视化 archetype + component |
| 测试覆盖 | ✅ 27 个测试文件 |

**结论：采用 DECS 做脚本侧 ECS，C++ EnTT 做物理模拟层，两者用窄接口桥接。自造 ECS API 的工作量从 ~2000 行降到 ~400 行，且获得热更新/序列化/调试免费。**

## 3. 整体架构

```
                          ┌──────────────────────────┐
                          │      MassiveModule       │   DasLang C++ Module
                          │   (Bridge: ~15 函数)     │
                          └─────┬──────────┬─────────┘
                                │          │
              ┌─────────────────┼──────────┼─────────────────┐
              │                 │          │                 │
         EnTT registry       DECS       DECS             DECS
         (C++ 独占写)      archetype  archetype        archetype
              │             │           │                 │
    ┌─────────┼──────┐  ┌───┼───┐  ┌───┼──────┐  ┌──────┼──────┐
    │Position │Health│  │Buff  │  │Skill      │  │AI        │
    │Velocity │Stats │  │State │  │Cooldown   │  │Blackboard│
    │Collider │Combat│  │DOT   │  │CastState  │  │PatrolData│
    └─────────┴──────┘  └──────┘  └───────────┘  └──────────┘
         ▲                   │          │              │
         │ 写入              │ 写入     │ 写入         │ 写入
    ┌────┴──────────┐   ┌────┴──────────┴──────────────┴────┐
    │ C++ Systems   │   │      Script Systems (DECS)        │
    │ (EnTT 操作)   │   │  query() / update_entity()        │
    └───────────────┘   └──────────────────────────────────┘
              │                          │
              │    MassiveModule         │
              └──────────┬───────────────┘
                         │
              ┌──────────┴──────────┐
              │  Bridge 窄接口       │
              │ entity_battlestats │
              │ entity_is_dead     │
              │ entities_in_radius │
              │ create/destroy       │
              │ send_to_client       │
              │ schedule_timer       │
              └─────────────────────┘
```

**核心原则**：
- **每份数据只有一个 writer**。不存在双缓冲、锁同步的复杂性。
- **EnTT 是物理模拟层**：Position, Velocity, Health, Collider, CombatTag → C++ 独占写入，脚本只读。
- **DECS 是游戏逻辑层**：BuffState, SkillCooldown, AIDecision, QuestProgress → 脚本独占写入，C++ 通过 Bridge 只读。
- **Bridge 不做 "ECS 查询"**。Bridge 是工具函数集——空间查询、属性读取、世界交互。

### 3.1 关于双缓冲的放弃

s13 原案设计了双缓冲（current/next swap），保证同一 Tick 内所有 entity 看到一致的世界快照。本方案**不做双缓冲**，理由：

| 考量 | 双缓冲（s13） | 本方案（无缓冲） |
|------|-------------|-----------------|
| 内存 | 2× EnTT registry + 2× BlobStorage ≈ 10MB（10000 entity） | 1×，无额外内存 |
| 一致性 | 严格：所有 entity 看到 N-1 帧快照 | 宽松：Stage 之间可见前面修改 |
| 复杂度 | copy-on-write 分摊逻辑 + swap 时序 | 简单线性执行 |
| 实际影响 | 对确定性回放有价值 | 对 MMO 业务无实际影响——同一 Tick 内 Stage 顺序保证确定，任何 entity 的计算基于"前面 Stage 已完成"的状态 |

**设计约束**：Stage 顺序必须正确反映数据依赖。例如 movement 必须在 combat 之前——因为 combat Stage 中的 AI 可能需要知道 entity 已经移动后的位置。这个排序在设计时确定，而非由双缓冲掩盖错误。

### 3.2 DECS Stage 的精确语义

> ⚠️ **待 Phase 1 实测验证**。以下分析基于 DECS 文档和行为推测，具体行为以 Phase 1 中实际运行结果为准。

`decs_stage("combat")` 是一个**声明式分组**：它运行所有标注了 `[decs(stage = "combat")]` 的 system。

预期行为（待验证）：
- **commit 发生在整个 Tick 结束时**（或显式调用 `commit()`），不是每个 Stage 之间。同一 Tick 内的所有 Stage 共享同一份 DECS world 视图。
- 这意味着 **Stage N 可以读到 Stage N-1 写入但未 commit 的数据**——因为它们在同一个 DECS world 上操作。

这个行为在逻辑上是可控的：Stage 顺序即是数据流顺序。如果需要在 Stage 之间隔离修改，可以在 Stage 函数末尾显式调用 `commit()`。

> **Phase 1.5 验证清单**：
> 1. `decs_stage()` 是否阻止跨 Stage 数据可见性？
> 2. `commit()` 的实际语义——是"刷新到内部持久化"还是"标记写完成"？
> 3. 多个 `decs_stage()` 调用是否共享同一个 DECS world？
> 4. Phase 1 完成后更新此段，将推测替换为实测结论。

## 4. 数据归属：EnTT vs DECS

### 4.1 判断标准

| 标准 | → EnTT（C++） | → DECS（脚本） |
|------|--------------|----------------|
| 每 Tick 遍历实体数 | >100 | <50 |
| 计算复杂度 | 简单线性（向量加法、距离计算） | 多分支状态机（行为树、任务链） |
| 与其他 C++ 系统的耦合 | 强（AOI 读 Position，碰撞读 Collider，战斗读 Health） | 弱（独立消费→产生 modifier） |
| 修改频率 | 每 Tick 稳定运行 | 条件触发（收到消息、定时器到期） |
| 热更新需求 | 无（物理不热更，改了就重启） | 高（策划日夜调数值、改 Buff 逻辑） |

### 4.2 组件分配表

```
EnTT (C++ 独占写)                  DECS (脚本独占写)
─────────────────────────────────  ─────────────────────────────────
Position          (float3)         BuffState         (buffId, ticks, dmg)
Velocity          (float3)         SkillCooldown     (skillId, remaining)
Health            (int32)          SkillCastState    (castTime, target)
Stats             (struct)         AIDecision        (state, timer)
Collider          (float radius)   AIBlackboard      (target, patrolIdx)
CombatTag         (tag)            QuestProgress     (questId, step, progress)
Level/XP          (int32/64)       TradeRecord       (partner, items, state)
MovementState     (enum)           DialogueState     (npcId, step, choices)
EntityType        (Player/NPC/     LootPermission    (allowedEids)
                   Monster/Pet)
```

### 4.3 特殊桥梁组件：HealthModifier

脚本**不直接写 EnTT Health**。C++ 是 Health 的权威 owner。脚本通过写入 DECS 的 `HealthModifier` 组件来影响战斗——C++ 的 `RecalcStats` system 每 Tick 汇总这些 modifier，一次性应用到 EnTT Health。

```
脚本 Tick:                                  C++ Tick:
  [DECS] damageBuff.flatDelta -= 10    →    [EnTT] for e in entities_with_modifier:
  [DECS] healBuff.flatDelta += 5       →        health.current += sumModifier(e)
                                                 clear_modifier(e)  // 下 Tick 重新累积
  → commit() 后 C++ 可读 modifier
```

**这只发生在 Tick 边界（C++ system 在脚本 commit 后运行），不存在并发问题。**

## 5. Bridge：MassiveModule C++ 绑定

### 5.1 函数清单

```cpp
// ===== Src/Common/ECS/MassiveModule.h =====
// DasLang Module——暴露给脚本的桥接函数

class MassiveModule : public das::Module
{
public:
    MassiveModule()
        : Module("massive")
    {
        ModuleLibrary lib(this);
        lib.addBuiltInModule();

        // 注册 C++ struct 到 DasLang 类型系统
        // BattleStats 通过 ManagedStructureAnnotation 整体暴露
        // 脚本侧直接读写 stats.attack / stats.defense / stats.critRate ...
        addAnnotation(new das::ManagedStructureAnnotation<BattleStats>("BattleStats", lib));

        // ── 1. 空间查询（AOI/范围技能/追击）──
        addExtern<DAS_BIND_FUN(massive_entity_position)>(...);
        //   float3 massive_entity_position(uint64 entityId)

        addExtern<DAS_BIND_FUN(massive_entities_in_radius)>(...);
        //   array<uint64> massive_entities_in_radius(float3 center, float radius)

        // ── 2. 属性查询 ──
        // 整体暴露 BattleStats——一次调用拿全部字段
        addExtern<DAS_BIND_FUN(massive_entity_get_battlestats)>(...);
        //   BattleStats? massive_entity_get_battlestats(uint64 entityId)
        //   → 返回 optional——entity 不存在或无 BattleStats 时返回 null

        // ── 3. Tag/State 查询——独立布尔函数，不用字符串 ──
        addExtern<DAS_BIND_FUN(massive_entity_is_dead)>(...);
        addExtern<DAS_BIND_FUN(massive_entity_is_in_combat)>(...);
        addExtern<DAS_BIND_FUN(massive_entity_is_stunned)>(...);
        addExtern<DAS_BIND_FUN(massive_entity_is_player)>(...);
        addExtern<DAS_BIND_FUN(massive_entity_is_monster)>(...);
        // 生产期按需追加（is_rooted / is_silenced / is_npc ...）

        // ── 4. 世界交互（创建/销毁/发消息）──
        addExtern<DAS_BIND_FUN(massive_create_entity)>(...);
        //   uint64 massive_create_entity(float3 pos, int32 entityType)

        addExtern<DAS_BIND_FUN(massive_destroy_entity)>(...);
        //   void massive_destroy_entity(uint64 entityId)

        addExtern<DAS_BIND_FUN(massive_send_to_client)>(...);
        //   void massive_send_to_client(uint32 sessionId, uint32 msgId,
        //                                array<uint8> data)

        addExtern<DAS_BIND_FUN(massive_broadcast_nearby)>(...);
        //   void massive_broadcast_nearby(float3 center, float radius,
        //                                  uint32 msgId, array<uint8> data)

        // ── 5. 定时器 ──
        addExtern<DAS_BIND_FUN(massive_schedule_timer)>(...);
        //   uint32 massive_schedule_timer(int32 delayMs, block<(timerId:uint32):void>)

        addExtern<DAS_BIND_FUN(massive_cancel_timer)>(...);
        //   void massive_cancel_timer(uint32 timerId)

        // ── 6. 工具 ──
        addExtern<DAS_BIND_FUN(massive_log_info)>(...);
        addExtern<DAS_BIND_FUN(massive_log_warn)>(...);
        addExtern<DAS_BIND_FUN(massive_log_error)>(...);
        addExtern<DAS_BIND_FUN(massive_get_dt)>(...);
        //   float massive_get_dt()    → 返回本 Tick 的 delta time（秒）

        addExtern<DAS_BIND_FUN(massive_find_entity_by_session)>(...);
        //   uint64 massive_find_entity_by_session(uint32 sessionID)
        //   → 查 _sessions 映射，返回 fullEntityId
    }
};
```

**共 18 个函数。不做通用 ECS 查询系统。全部静态编译期绑定——零字符串、零 `strcmp`。**

### 5.2 为什么没有 `has_tag(entity, "Dead")` 和 `entity_stat(entity, "attack")`

原案使用字符串参数（`string tag`、`string statName`），经评审否决——改为独立编译期函数。理由：

| 问题 | 字符串方案 | 独立函数方案 |
|------|-----------|-------------|
| 运行时开销 | `strcmp` + `if-else` 链 | 一次 `all_of<T>()` → O(1) |
| 拼写错误 | `"Deed"` / `"dead"` 不报错 | 函数不存在→编译期错误 |
| Tag 废弃 | 删除 C++ 的 `StunnedTag` → 脚本写 `"Stunned"` 不报错 | 删除 Bridge 函数→编译期错误 |
| 新增成本 | 加一行 `strcmp` | 加一个 3 行函数 + `addExtern` |
| BattleStats 查询 | 每次一个字段，多次函数调用 | 一次 `get_battlestats()` 拿全部 `.attack` `.defense` `.critRate` |

**Bridge 只处理跨边界的问题**（空间查询、Tag 判断、网络 IO、定时器），组件级别的读写全部在 DECS 内部完成。脚本通过 `query()` / `update_entity()` 操作 DECS component，不需要经过 Bridge。

### 5.3 DasLang ↔ C++ 类型映射表

下面是 `MassiveModule::addExtern` 绑定时需要用到的类型映射关系：

| 概念 | DasLang 端写法 | C++ 端实现类型 | 说明 |
|------|---------------|---------------|------|
| float3 三元组 | `float3(x,y,z)` | `das::float3`（已内置） | daScript 内建类型，直接可用 |
| uint32 数组 | `array<uint32>` | `das::Array`（模板） | 注意不是 `vector<uint32>`——das::Array 才有 ref-count GC |
| uint64 数组 | `array<uint64>` | `das::Array` | 同上 |
| uint8 数组 | `array<uint8>` | `das::Array` | Protobuf body 序列化为 `array<uint8>` |
| block 闭包 | `block<(args):ret>` | `das::TBlock<void, Args...>` | 定时器回调等需要持有闭包 |
| string | `string` | `das::TBlock<char>` / `char*` | das::string 是内置管理的 |
| table<> | `table<uint32; block<...>>` | — | DasLang 内置关联容器，C++ 一般不直接操作 |
| DECS EntityId | `EntityId{id; generation}` | — | DECS 内部结构，脚本侧不直接暴露给 Bridge |

**关键约束**：
- `das::Array` 是引用计数 GC 管理的，C++ 构造函数需要指定 `Context*`。Bridge 函数通过 `Module` 的内部 context 自动获得。
- 复杂 struct（如 `float3` 的包装）可以直接用 daScript 内置类型，不需要自定义 ManagedStructureAnnotation。
- `block<>` 闭包的 C++ 模板参数是 `das::TBlock<ReturnType, ArgTypes...>`，通过 `das_invoke` 在 LogicThread 中同步执行。

### 5.4 Bridge Channel B：C++ 读取 DECS 状态

§5.1 定义的 15 个函数只覆盖了"脚本调用 C++"这个方向。但 §11 `RecalcStats` 需要**反向的桥接**——C++ 遍历 DECS 中的 `HealthModifier` 组件。这部分不在 §5.1 的 Bridge 函数清单中。

双向桥接通过 `ScriptBridge` 辅助类完成：

```cpp
// Src/Common/ECS/ScriptBridge.h —— C++ 读取 DECS 状态的辅助工具
class ScriptBridge
{
public:
    explicit ScriptBridge(das::ContextPtr ctx);

    // 查询 DECS 中拥有指定组件的所有 entityID 列表
    std::vector<uint32> GetEntitiesWithComponent(const char *componentName);

    // 读取指定 entity 的 DECS 组件值（模板特化每种组件类型）
    template <typename T>
    std::optional<T> GetComponentValue(uint32 entityID, const char *componentName);

    // 清空指定 entity 的 DECS 组件
    void ClearComponent(uint32 entityID, const char *componentName);

private:
    das::ContextPtr _ctx;
};
```

**实现路径**：ScriptBridge 通过调用 DECS 暴露给 C++ 的辅助函数工作——DECS 的 `.das` 文件中需要导出几个 `[export]` 函数：

```das
// decs_bridge.das —— 在 Scripts/ 目录下
require daslib/decs_boost

[export]
def get_entities_with_component(componentName : string) : array<uint32>
{
    var result : array<uint32>
    query() $(var cmp : auto(componentName)) {
        let eidVal = get_eid()  // DECS 内置的 entityID 获取
        push(result, eidVal)
    }
    return result
}

[export]
def get_component_bytes(entityID : uint32; componentName : string) : array<uint8>
{
    // 读取 component 的 raw bytes，C++ 端通过 reinterpret_cast 解析
    ...
}
```

> **优先级**：`ScriptBridge` 的实现放在 Phase 2（与 `RecalcStatsSystem` 一起），Phase 1 不需要——只需在 `MassiveModule` 中预留位置。Phase 3 后评估是否需要进一步扩展。

## 6. Entity 统一 ID 空间

### 6.1 问题

DECS 和 EnTT 各自维护 entity ID 和生命周期。两个独立 ID 空间无法互操作。

### 6.2 方案：EnTT 是权威，DECS 用相同 ID

```cpp
// EntityManager — 统一 entity 创建入口
class EntityManager
{
public:
    // 创建 entity：同时在 EnTT 和 DECS 中分配 ID
    // 返回 uint64 = (sceneId << 32) | entityId
    uint64 CreateEntity(Scene& scene, const float3& pos, EntityType type);

    // 销毁 entity：先通知 DECS（清理所有组件），再销毁 EnTT entity
    void DestroyEntity(Scene& scene, uint64 fullEntityId);

private:
    std::atomic<uint32> _nextLocalId{1};
};
```

**关键设计**：
- **DECS 的 entity ID 就是 EnTT 的 entity ID（uint32）**。不需要双向映射表。
- 对外（脚本、网络）使用 `uint64` = `(sceneId << 32) | entityId`，与现有 `Entity.h` 兼容。
- Entity 创建必须通过 `EntityManager`（唯一入口），不能绕过它直接调 `registry.create()` 或 `decs::create_entity()`。

### 6.3 精确实现：ID 与 Generation 的同步

DECS 使用 `EntityId{id: uint, generation: uint}` 结构，其中 `generation` 用于检测 dangling reference（entity 销毁后旧 ID 失效）。EnTT 同样有 `entt::entity` 的 version 概念。两者的 generation/version 必须同步。

```cpp
// EntityManager::CreateEntity() 的精确实现
uint64 EntityManager::CreateEntity(Scene& scene, const float3& pos, EntityType type)
{
    // 1. EnTT 创建（权威）
    entt::entity e = scene.Registry().create();
    uint32 entityID = static_cast<uint32>(entt::to_integral(e));
    //    entt::to_integral 返回 (version << 32) | entity_id
    //    entity_id 在低 32 位，version 在高 32 位（取决于 ENTT_SPARSE_PAGE）
    //    实际使用：取低 32 位作为 entity_id，取高 32 位作为 generation

    // 2. 从 EnTT entity 提取 generation（用于 DECS EntityId）
    uint32 generation = scene.Registry().current(e);  // version counter

    // 3. 设置 EnTT 组件
    scene.EmplaceComponent<Position>(Entity{scene.SceneID(), entityID}, pos.x, pos.y, pos.z);
    scene.EmplaceComponent<EntityTypeTag>(Entity{scene.SceneID(), entityID}, type);

    // 4. DECS 创建（同一 ID）——通过 Bridge 让脚本创建
    //    脚本调 create_entity() 时手动指定 id 和 generation：
    //      let e = create_entity(eid = EntityId{id=entityID; generation=generation})
    //    这个调用通过 massive_create_entity 的返回值触发
    //    或者在 MassiveModule 中提供一个专门的 bridge 函数来同步 DECS 侧

    // 5. 返回统一 ID
    return (static_cast<uint64>(scene.SceneID()) << 32) | entityID;
}
```

**DECS 侧创建**：Bridge 提供一个 `massive_decs_create_entity(entityID, generation)` 辅助函数，它在 DECS world 中创建与 EnTT 相同 ID 的 entity。

```
massive_create_entity(pos, type) 调用流程：
  C++: EnTT.create() → entityID, generation
  C++: 调用 massive_decs_create_entity(entityID, generation)
       └→ 脚本侧: decs::create_entity(id=entityID, gen=generation)
  C++: 返回 uint64(sceneID << 32 | entityID)

massive_destroy_entity(fullEntityId) 调用流程：
  C++: 调用 massive_decs_destroy_entity(entityID)
       └→ 脚本侧: decs::destroy_entity(EntityId{entityID, generation})
  C++: EnTT.destroy(entt::entity(entityID))
```

### 6.4 关键边界场景：ID 复用

Entity 被销毁 → EnTT 可能复用同一个 entity ID（下一次 `create()`），但 **generation/version 会递增**。这意味着：

- DECS 中的旧 `EntityId{id=42, generation=5}` 与新 `EntityId{id=42, generation=6}` 是不同的实体
- `create_entity()` 时必须指定 generation，否则 DECS 会认为是同一个 entity
- EnTT 的 `registry.valid(e)` 和 DECS 的 `is_alive()` 必须同时检查

**Phase 2 测试的重点场景**：
1. 创建 entity A (id=1, gen=1)
2. 销毁 entity A
3. 立即创建 entity B（id=1, gen=2）
4. 验证 DECS 侧 entity B 是干净的（无 entity A 的残留组件）
5. 验证旧引用 (id=1, gen=1) 在 DECS 中 `is_alive()` 返回 false

### 6.5 脚本侧 Entity 引用策略

脚本中的 entity 引用使用 `uint64`（与 C++ Entity 结构对齐），不直接暴露 DECS 的 `EntityId` 给 Bridge API：

- 脚本调用 `massive_create_entity(pos, type)` 返回 `uint64`
- 这个 ID 同时作为 DECS 组件中的 `ownerEid` 字段存储
- DECS 内部的 `"eid"` 组件（`EntityId` 结构）通过 `entityID → generation` 映射表查找

Bridge 需要维护一个轻量的 entityID → generation 映射（`std::unordered_map<uint32, uint32>`），在 EnTT `on_construct` / `on_destroy` 回调中更新。

## 7. Protobuf 反序列化策略

### 7.1 问题

DasLang 没有原生 Protobuf 支持。如果脚本需要访问 `MoveReq.targetPos`，必须解决反序列化问题。两种可行方案：

### 7.2 方案 A（推荐）：C++ 预解析

**C++ 侧完成 Protobuf 反序列化，将已解码的消息以 DasLang 原生类型传给脚本 handler。**

```
消息流：
  Gate → IO Thread → Per-Session inbox → LogicThread::ProcessMessages()
                                              │
                                    C++ MessageDispatcher
                                    (protobuf 反序列化)
                                              │
                                    已解码 struct → 脚本 handler
```

**脚本侧**：
```das
// 不需要任何 protobuf 依赖——handler 接收原生 DasLang 类型
msg_handlers[MSG_MOVE_REQ] <- @(sessionId: uint32; pos: float3; timestamp: uint64)
{
    let playerEid = find_player_by_session(sessionId)
    query(playerEid) $(var moveIntent : MoveIntent) {
        moveIntent.targetPos = pos
        moveIntent.timestamp = timestamp
    }
}
```

**C++ 分派侧**：
```cpp
void WorldServer::OnMessage(uint32 sessionID, WorldSession& ws, const LogicMessage& msg)
{
    // 控制消息留在 C++ 处理
    if (msg.msgID < kUserMsgIDStart) {
        OnControlMessage(msg.msgID, msg.body.data(), msg.body.size());
        return;
    }

    // 业务消息——C++ 预解析，投递原生类型给脚本
    switch (msg.msgID) {
    case MSG_MOVE_REQ: {
        MoveReq req;
        req.ParseFromArray(msg.body.data(), msg.body.size());
        auto fn = _scriptCtx->findFunction("handle_move");
        das_invoke<void>::invoke(_scriptCtx.get(), nullptr, fn,
            sessionID, req.x(), req.y(), req.z(), req.timestamp());
        break;
    }
    case MSG_LOGIN_ENTER_WORLD_REQ: {
        EnterWorldReq req;
        req.ParseFromArray(msg.body.data(), msg.body.size());
        auto fn = _scriptCtx->findFunction("handle_enter_world");
        das_invoke<void>::invoke(_scriptCtx.get(), nullptr, fn,
            sessionID, req.account_id(), req.token(), req.scene_id(),
            req.pos_x(), req.pos_y(), req.pos_z());
        break;
    }
    default:
        break;
    }
}
```

**优势**：
- 脚本零 Protobuf 依赖，学习成本低
- C++ 的反序列化是零拷贝的（protobuf-lite）
- 消息类型安全——编译期检查，不会因为脚本写了错的字段名而在运行时崩溃

**代价**：
- 每个新消息类型需要在 C++ 侧写一行 switch case + `ParseFromArray`
- 消息字段变更时 C++ 侧也需要同步更新分派代码

### 7.3 方案 B（备选）：Protobuf-Lite 绑定

将 protobuf-lite 反序列化能力绑定到 DasLang Module，脚本直接解析 raw bytes。

**不推荐**：增加脚本复杂度、DasLang 绑定工作量、运行时字段名查错困难。

### 7.4 决策

**采用方案 A**。C++ switch case 由 `GenMsgBindings.py` 自动生成（Phase 2 产出），不手工维护。生成器实时扫描 `.proto` 文件，产出 `MsgDispatch.gen.cpp` + `MsgArgs.gen.h` + `HandlerRegistry.das` + `MsgIDConstants.das` 四个文件。

## 8. LogicThread 集成

### 8.1 启动流程

```
WorldServer::Init()
  ├── 1. 创建 Scene（EnTT + NavMesh + AOI）
  ├── 2. 启动 LogicThread
  ├── 3. 初始化 DasLang Context
  │       ├── compileDaScript("Scripts/ServerTick.das")
  │       ├── program->simulate(ctx)
  │       └── ctx->restart()  // 首次初始化
  ├── 4. 调用脚本 init()
  │       ├── restart()          // 清空 DECS 世界
  │       ├── spawn_initial_npcs()
  │       └── commit()
  └── 5. 接受客户端连接
```

### 8.2 每 Tick 调度

> **注意**：下面"Phase N"的编号对应本文档自身的逻辑分层，**不是** `LogicThread::RunLoop()` 中的 Phase 注释编号。实际代码的 `onTick(budget)` 在 RunLoop 中标注为 "Phase 4: 游戏逻辑"。

```cpp
// LogicThread::RunLoop() 中——实际代码结构（详见 LogicThread.cpp）
void LogicThread::RunLoop(...) {
    while (!_stopped) {
        auto tickStart = std::chrono::steady_clock::now();

        // Phase 0: 动态入口门控（消息预算自适应）
        size_t msgLimit = _currentMsgLimit;

        // Phase 1: 预处理（RPC 超时等）
        preProcess();

        // Phase 2: 消息处理（Per-Session inbox Drain）
        ProcessMessages(sessions, sessionsMtx, onMessage, msgLimit);

        // Phase 3: 定时器 Tick
        _timingWheel.Tick();

        // Phase 4: DB 回调
        DBWorkerPool::Instance().ProcessCallbacks();

        // Phase 5: 游戏逻辑 —— 传剩余时间预算（ms），不是 dt
        auto budget = kTickInterval - (now - tickStart);  // 至少 5ms
        onTick(std::chrono::duration_cast<std::chrono::milliseconds>(budget));

        // Phase 6: 出站刷新
        postFlush();

        // sleep 到下一个 20ms 窗口
        sleepRemaining();
    }
}
```

**当前 `OnTick` 回调的实际内容**（`WorldServer::OnTick()`）：

```cpp
void WorldServer::OnTick(std::chrono::milliseconds budget) {
    // 1. 处理未路由的 EnterWorldReq
    ProcessUnroutedMessages();
    // 2. 控制消息（DisconnectNtf / SessionRebindReq）
    ProcessControlMessages();
    // 3. 过载保护检查
    UpdateLoadLevel(...);
    // 4. (TODO) 游戏逻辑——此处将接入脚本 Tick
    //    即本文档的 §8.2 Target State 中描述的脚本 Stage 调度
}
```

**集成脚本后将变为（Target State）**：

```cpp
void WorldServer::OnTick(std::chrono::milliseconds budget) {
    ProcessUnroutedMessages();
    ProcessControlMessages();
    UpdateLoadLevel(...);

    // ── 脚本 Tick ──
    // dt 用固定值 20ms（或实际 elapsed），而非 budget
    // budget 是剩余时间配额，不是帧间隔
    constexpr float dt = 0.02f;  // 20ms
    das_invoke<void>::invoke(_scriptCtx.get(), nullptr, fnUpdate, _defaultSceneID, dt);

    // ── C++ Systems（在脚本 commit 后运行）──
    RunCPPSystems(scene, dt);
}
```

**脚本 Tick 的详细内容**（`Scripts/ServerTick.das`）：

```das
[export]
def update(sceneId : uint32; dt : float)
{
    decs_stage("input")       // → 处理消息队列中的业务消息
    decs_stage("buff_tick")   // → DoT/HoT 推进 + 过期移除
    decs_stage("skill_cd")    // → 技能冷却递减
    decs_stage("ai_decision") // → 行为树 Tick
    decs_stage("quest_check") // → 任务进度检查
    decs_stage("dirty_flush") // → 脏数据标记（配合 C++ 网络复制）
}
```

**Phase 7 C++ Systems 执行顺序**（在脚本 commit 后，EnTT 可安全读取 DECS 状态）：

```cpp
void RunCPPSystems(Scene& scene, float dt) {
    SystemMovement(scene, dt);        // EnTT Position += Velocity * dt
    SystemRecalcStats(scene);         // 汇总 DECS HealthModifier → 应用到 EnTT Health
    SystemAOI(scene);                 // 空间索引更新
    SystemReplicate(scene);           // 网络复制——差量序列化
}
```

### 8.3 定时器回调的安全约束

脚本的 `schedule_timer(delayMs, block)` 回调在 C++ TimingWheel Tick（RunLoop Phase 3）触发，**运行在 LogicThread 上**。这意味着：

- 定时器回调可以和 DECS query 安全交互（同一线程）
- 回调中不能访问正在迭代的 C++ EnTT 数据（避免 re-entrancy）

> **备注**：定时器回调在 RunLoop Phase 3 执行，而脚本 Tick 在 Phase 5 执行。定时器回调中对 DECS 的修改在 Phase 5 脚本 Tick 的 `decs_stage()` 中可见（同一 DECS world）。
> 这意味着定时器修改 DECS 组件后，同一次 RunLoop 迭代中的脚本 Tick 能立即看到这些修改——不需要跨 Tick 等待。

## 9. 热更新

### 9.1 机制

```cpp
// WorldServer 暴露的脚本重载入口（GM 指令 / HTTP endpoint）
void WorldServer::ReloadScript()
{
    // 1. 保存 DECS 状态（decs_live 自动处理）
    // 2. 重启 Context
    _scriptCtx->restart();
    // 3. 重新编译
    auto newProgram = compileDaScript("Scripts/ServerTick.das", ...);
    if (newProgram->failed()) {
        Log::Error("Script reload failed: {}", ...);
        return;  // 旧代码继续运行
    }
    // 4. 热替换
    _scriptCtx->relocateCode(newProgram);
    // 5. 恢复 DECS 状态 + 重新调用 init()
    das_invoke<void>::invoke(_scriptCtx.get(), nullptr, fnInit);
    Log::Info("Script reloaded successfully");
}
```

### 9.2 什么能热更，什么不能

| 更新内容 | 操作 | 状态影响 |
|----------|------|---------|
| DECS Stage 函数实现（修改 Buff 伤害公式） | 保存 .das → auto-reload | 无影响——DECS 组件数据不受代码影响 |
| DECS struct 增加字段 | 保存 + 热重载 | 旧实体保留旧字段值，新字段填默认值 |
| DECS struct 删除字段 | 保存 + 热重载 | 反序列化时忽略多余字节（Archive 处理） |
| C++ Component 定义 | 改 .h → 重启服务器 | 所有实体丢失——需完整重启 |
| ecs_stage 调度顺序 | 改 `update()` → 热重载 | 即刻生效 |
| 新增 Stage 函数 | 加 `[decs(stage=...)]` → 热重载 | 即刻生效 |
| C++ 消息分派 switch case | 改 .cpp → 重启服务器 | 新增消息类型需重启 |

### 9.3 热更新期间的数据流

```
[before_reload]  ← decs_live: mem_archive_save(decsState)
     │
shutdown()       ← 脚本清理（unregister listeners 等）
     │
restart()        ← Context 重启
     │
重新编译         ← 新 .das 文件
     │
[after_reload]   ← decs_live: mem_archive_load(data, decsState)
     │
init()           ← 脚本重新初始化（仅非首次启动时用 is_reload() 判断）
     │
update() 继续    ← 正常 Tick
```

在 init() 中区分首次启动和热重载：

```das
var g_isFirstInit = true

[export]
def init()
{
    if g_isFirstInit
    {
        // 首次启动：spawn NPC、加载初始数据
        spawn_initial_npcs()
        g_isFirstInit = false
    }
    // 热重载：decs_live 已恢复组件数据，只需重新注册事件监听器
    register_handlers()
    register_event_listeners()
}
```

## 10. 消息处理：从 C++ Handler 迁移到脚本

### 10.1 当前状态

`EnterWorldHandler.cpp` 和 `MoveHandler.cpp` 在 C++ 中直接操作 EnTT。迁移后，这些 Handler 变为**脚本中的 `on_message` 回调**。鉴于本项目尚未堆积大量 C++ Handler（仅 2 个），可以从 Day 1 就构建脚本 handler。

### 10.2 消息流

```
Client → Gate → World IO Thread → Per-Session inbox → LogicThread
                                                         │
                                             ProcessMessages()
                                                         │
                               msgID → C++ switch case → Protobuf 反序列化
                                                         │
                                       已解码的原生类型 → 脚本 handler
                                                         │
                                [DECS] handle_move(sessionId, x, y, z, timestamp)
                                [DECS] handle_enter_world(sessionId, accountId, token, ...)
                                ...
```

### 10.3 脚本侧消息处理示例

```das
// scripts/Handlers.das
require daslib/decs_boost
require massive

// 消息 ID 常量
let MSG_LOGIN_ENTER_WORLD_REQ = 102u
let MSG_MOVE_REQ = 200u

// 消息处理器注册表
var msg_handlers : table<uint32; block<(sessionId:uint32):void>>

[export]
def register_handlers()
{
    // EnterWorld
    msg_handlers[MSG_LOGIN_ENTER_WORLD_REQ] <- @(sessionId:uint32)
        handle_enter_world(sessionId, _accountId, _token, _sceneId, _posX, _posY, _posZ)

    // Move
    msg_handlers[MSG_MOVE_REQ] <- @(sessionId:uint32)
        handle_move(sessionId, _posX, _posY, _posZ, _timestamp)
}

// 具体的业务 handler 从 C++ 侧的被调用入口接收参数
def handle_enter_world(
    sessionId: uint32;
    accountId: uint64;
    token: string;
    sceneId: uint32;
    posX, posY, posZ: float
)
{
    let eid = massive_create_entity(float3(posX, posY, posZ), ENTITY_TYPE_PLAYER)

    create_entity() @(eid2, cmp) {
        cmp.eid := eid
        cmp.sessionId := sessionId
        cmp.accountId := accountId
    }
    commit()

    // 构建 EnterWorldRsp protobuf body → 发送
    massive_send_to_client(sessionId, MSG_LOGIN_ENTER_WORLD_RSP,
        build_enter_world_rsp(eid, posX, posY, posZ))
}

def handle_move(
    sessionId: uint32;
    x, y, z: float;
    timestamp: uint64
)
{
    // 服务器权威校验在 C++ SystemMovement 中完成
    // 脚本只需记录客户端意图
    let playerEid = find_player_by_session(sessionId)
    query(playerEid) $(var moveIntent : MoveIntent) {
        moveIntent.targetPos = float3(x, y, z)
        moveIntent.timestamp = timestamp
    }
}
```

### 10.5 Session → Player Entity 映射

`handle_move` 和 `handle_enter_world` 中需要从 `sessionID` 查找对应的 entity。映射存储方案：

**在 C++ 侧维护，通过 Bridge 提供查找**：

```cpp
// WorldServer 中的 session → entity 映射已经存在：
//   _sessions[sessionID].entity  → Entity{sceneID, entityID}
//
// 新增 Bridge 函数暴露给脚本：
addExtern<DAS_BIND_FUN(massive_find_entity_by_session)>(...);
//   uint64 massive_find_entity_by_session(uint32 sessionID)
//   → 查 _sessions，返回 uint64(sceneID << 32 | entityID)
```

**不在 DECS 中存 `(sessionID → entity)` 映射**——DECS 组件中的 `sessionID` 字段仅用于 C++ 侧从 entity 反查 session（如断开连接时清理场景）。双方向映射各维护一份，避免"谁是最新数据"的竞态。

### 10.6 脚本侧 Protobuf 序列化

`massive_send_to_client` 接收 `array<uint8>`，脚本需要能构造 protobuf body。两种方案：

**方案 A（Phase 3 采用）**：C++ 侧提供辅助函数

```cpp
addExtern<DAS_BIND_FUN(massive_build_enter_world_rsp)>(...);
//   array<uint8> massive_build_enter_world_rsp(uint64 entityID,
//                                              float posX, float posY, float posZ)
//   → C++ 内部用 protobuf-lite 构造 EnterWorldRsp，返回序列化后的 array<uint8>
```

每个需要脚本发送的消息类型对应一个 C++ Bridge 函数。消息种类有限（< 20 个出站消息），手工维护可行。

**出站 Protobuf 构造同样由 `GenMsgBindings.py` 自动生成。** 每个出站消息（Rsp/Ntf 后缀）生成对应的 `massive_build_{msg_name}` C++ 函数和 DasLang 声明。参见 [13_MessageMigration §11](13_MessageMigration.md)。

### 10.7 DECS Template 定义（Components.das）

§10.3 中 `create_entity() @(eid2, cmp)` 引用了尚未定义的 DECS template。在 `Scripts/Components.das` 中定义：

```das
// Scripts/Components.das —— 所有 DECS 组件定义（decs_template struct）
require daslib/decs

// Entity 标识（映射 C++ Entity{sceneID, entityID} 的 uint64 表示）
[decs_template]
struct EntityRef {
    eid : uint64     // = (sceneID << 32) | entityID
}

// 玩家额外数据
[decs_template]
struct PlayerData {
    sessionID : uint32
    accountID : uint64
}

// 移动意图（客户端上报，服务器权威校验后应用）
[decs_template]
struct MoveIntent {
    targetPos : float3
    timestamp : uint64
}

// ── Phase 3+ 的组件定义（后续迭代补充字段）──
// [decs_template] struct BuffState { ... }
// [decs_template] struct SkillCooldown { ... }
// [decs_template] struct HealthModifier { flatDelta : int; pctDelta : float }
// [decs_template] struct AIDecision { state : int; timer : float }
// [decs_template] struct AIBlackboard { target : uint64; patrolIdx : int; ... }
```

**注意**：DECS template 中的字段类型必须用 DasLang 原生类型——`uint64`、`uint32`、`float3`、`int` 等。`float3` 是 daScript 内建类型，DECS 原生支持。

```cpp
// WorldServer.cpp — 消息到达 LogicThread 后
void WorldServer::OnMessage(uint32 sessionID, WorldSession& ws, const LogicMessage& msg)
{
    // 控制消息（Gate 断线通知等）留在 C++ 处理
    if (msg.msgID < kUserMsgIDStart) {
        OnControlMessage(msg.msgID, msg.body.data(), msg.body.size());
        return;
    }

    // 业务消息——C++ 预解析 Protobuf → 投递原生类型给脚本
    // 注意：此处为示意代码，实际实现建议用注册表模式而非大 switch
    switch (msg.msgID) {
    case MSG_MOVE_REQ: {
        MoveReq req;
        if (!req.ParseFromArray(msg.body.data(), msg.body.size())) {
            Log::Warn("MoveReq parse failed for session {}", sessionID);
            return;
        }
        DasInvoke("handle_move", sessionID,
            req.x(), req.y(), req.z(), req.timestamp());
        break;
    }
    case MSG_LOGIN_ENTER_WORLD_REQ: {
        EnterWorldReq req;
        if (!req.ParseFromArray(msg.body.data(), msg.body.size())) {
            Log::Warn("EnterWorldReq parse failed for session {}", sessionID);
            return;
        }
        DasInvoke("handle_enter_world", sessionID,
            req.account_id(), req.token(), req.scene_id(),
            req.pos_x(), req.pos_y(), req.pos_z());
        break;
    }
    // ... 更多消息类型
    default:
        Log::Debug("Unhandled business msg: msgID={}", msg.msgID);
        break;
    }
}
```

## 11. Entity Health 的特殊桥接

如前所述，Health 在 EnTT 中（C++ 独占写），脚本通过 DECS `HealthModifier` 间接影响。C++ 的 `SystemRecalcStats` 是桥接点：

```cpp
// Src/World/System/RecalcStatsSystem.cpp（C++ System）
void SystemRecalcStats(Scene& scene)
{
    auto& registry = scene.Registry();
    auto  decsCtx  = scene.GetScriptContext();  // 获取 DasLang Context

    // 遍历 DECS 中所有有 HealthModifier 的实体
    // → 通过 Bridge 查询（C++ 读取 DECS 状态）
    auto modifiedEids = decsCtx->getEntitiesWithComponent("healthModifier");

    for (auto eid : modifiedEids) {
        if (!registry.valid(entt::entity(eid))) continue;

        // 读取 DECS 组件值
        auto modifier = decsCtx->getComponentValue<HealthModifier>(eid, "healthModifier");

        // 应用到 EnTT Health
        auto& health = registry.get<Health>(entt::entity(eid));
        health.current += modifier.flatDelta;
        health.current = std::clamp(health.current, 0, health.max);

        // 清空 modifier（下 Tick 重新累积）
        decsCtx->clearComponent(eid, "healthModifier");

        // 标记脏数据（网络复制）
        scene.MarkDirty<Health>(Entity{scene.SceneID(), eid});
    }
}
```

## 12. 错误处理与恢复策略

### 12.1 脚本异常处理

DasLang 通过 `panic` 机制处理运行时错误。脚本层异常不应导致服务器崩溃。

| 场景 | 处理策略 |
|------|---------|
| DasLang 编译错误 | `program->failed()` → Error 日志 + 保留旧代码继续运行 |
| 脚本运行时 panic（除零、越界等） | Context 级的 try/catch → Error 日志 + 行号 + 跳过当前 handler，不影响其他消息 |
| DECS Stage 中途异常 | 当前 Stage 中止，已修改的 component 保留（DECS 无回滚机制；Stage 内的修改是增量式的，不会产生数据损坏） |
| Bridge 函数调用失败（如 entity 不存在） | 返回哨兵值（-1, empty array）→ 脚本侧检查返回值 |

### 12.2 启动失败策略

```cpp
void WorldServer::InitScriptEngine()
{
    auto program = compileDaScript("Scripts/ServerTick.das", ...);
    if (program->failed()) {
        Log::Error("Script compilation failed: {}", ...);
        // 首次启动编译失败 = 致命错误，无法继续
        throw std::runtime_error("Script engine initialization failed");
    }
    // ...
}
```

### 12.3 热重载失败策略

```cpp
void WorldServer::ReloadScript()
{
    auto newProgram = compileDaScript("Scripts/ServerTick.das", ...);
    if (newProgram->failed()) {
        Log::Error("Script reload failed, keeping old code: {}", ...);
        return;  // 旧代码继续运行，服务不中断
    }
    _scriptCtx->relocateCode(newProgram);
    // ...
}
```

### 12.4 降级开关（Feature Flag）

在开发和生产环境之间，C++ Handler 作为 fallback：

```cpp
// 配置项
bool _useScriptHandlers = true;  // 可通过 TOML 配置动态调整

void WorldServer::OnMessage(uint32 sessionID, WorldSession& ws, const LogicMessage& msg)
{
    if (_useScriptHandlers && msg.msgID >= kUserMsgIDStart) {
        DispatchToScript(sessionID, msg);
    } else {
        DispatchToNative(sessionID, msg);
    }
}
```

## 13. 调试与可观测性

### 13.1 DECS 状态可视化

DECS 内置 `decs_state` 支持在 DAP debugger（VS Code / Visual Studio）中可视化 archetype 和 component：

```
// 在 DAP debugger 的 Watch 窗口中
> decs_state
→ archetypes: 12, entities: 847
  archetype "Position+Velocity+MonsterTag": 320 entities
  archetype "Health+CombatTag": 500 entities
  archetype "BuffState": 27 entities
```

### 13.2 脚本性能分析

**生产环境 — Tracy（C++ 侧）**：

```cpp
void LogicThread::RunLoop(...) {
    while (!_stopped) {
        MASSIVE_FRAME_MARK();
        {
            MASSIVE_PROFILE_NAME("ScriptTick");
            onTick(budget);  // → das_invoke update()
        }
        {
            MASSIVE_PROFILE_NAME("CPPSystems");
            RunCPPSystems(scene, dt);
        }
    }
}
```

**开发/调试期 — daScript Profiler（脚本侧自动埋点）**：

daScript 0.6.3+ 内置了 `profiler.das`（[22_DaScript063Analysis](22_DaScript063Analysis.md) §3）— 自动挂钩所有 daScript 函数调用记录 PerfEvent 时间线，输出 Chrome Trace JSON。相比 Tracy 手动 `MASSIVE_PROFILE_NAME()`，profiler.das 自动覆盖每个 handler、每个 DECS Stage、每个 AI 决策的精确耗时。

```
# 开发时启动 server 追加 profiler 参数:
./WorldServer --das-profiler-log-file=logs/script_trace.json

# 停止后用 chrome://tracing 打开 script_trace.json:
→ AIBlackboard query: 8.3μs (avg)
→ handle_move:          2.1μs
→ system_buff_tick:    45.7μs (100 entities × 5 buffs)
→ system_skill_cd:     12.4μs
```

> **不需要手写 `massive_profile_begin/end()` Bridge 函数。** `profiler.das` 已自动挂钩所有脚本函数——C++ Tant 埋点保留用于 Tick 总耗时和 CPPSystems 开销，脚本级细节由 profiler.das 覆盖。

### 13.3 Metrics 指标

```cpp
// Prometheus 指标（snake_case）
_metrics.RegisterGauge("massive_script_entity_count",  [&] { return decsCtx->entityCount(); });
_metrics.RegisterGauge("massive_script_stage_duration_ms", [&] { return _lastScriptTickMs.load(); });
_metrics.IncrementCounter("massive_script_panic_total");
_metrics.IncrementCounter("massive_script_reload_total");
```

## 14. 目录结构

```
Massive/
├── Scripts/                          # 新建——DasLang 脚本
│   ├── ServerTick.das                # 顶层 Tick 调度 + init/shutdown
│   ├── Handlers.das                  # 消息处理器注册 + 业务 Handler
│   ├── Components.das                # DECS 组件定义（decs_template structs，§10.7）
│   ├── DecsBridge.das                # C++ 读取 DECS 状态的脚本侧 bridge（§5.4）
│   ├── Buff.das                      # Buff 系统（DECS Stage）[Phase 4]
│   ├── Skill.das                     # 技能 CD + 施法管线 [Phase 4]
│   ├── AI.das                        # AI 行为树节点 + Stage [Phase 4]
│   └── Quest.das                     # 任务进度检查 [Phase 4+]
│
├── ThirdParty/                       # 第三方库
│   ├── daScript/                     # ✅ 已存在——核心运行时 + 标准库
│   │   ├── include/                  #   C++ 头文件（das::Context 等）
│   │   └── daslib/                   #   ✅ DECS 在此（纯 .das 脚本）
│   │       ├── decs.das              #   DECS 核心
│   │       ├── decs_boost.das        #   DECS 增强（query 宏、Stage 调度）
│   │       └── decs_state.das        #   DAP debugger 可视化
│
├── Src/Common/ECS/
│   ├── MassiveModule.h               # 新建——DasLang C++ Module（15 个桥接函数）
│   ├── MassiveModule.cpp
│   ├── EntityManager.h               # 新建——统一 Entity 创建/销毁入口
│   ├── EntityManager.cpp
│   ├── ScriptBridge.h                # 新建——C++ 读取 DECS 状态的辅助工具
│   ├── Scene.h                       # 修改——增加 DasLang Context 引用
│   ├── Scene.cpp
│   ├── Entity.h                      # 保留——(sceneId, entityId) 不变
│   ├── DirtyTracker.h                # 保留——网络复制脏标记
│   ├── ScriptComponentStorage.h      # 逐步废弃——Phase 3 后评估是否删除
│   └── ScriptComponentStorage.cpp    #（被 DECS 替代，但 C++ RecalcStats 可能仍需要）
│
├── Src/World/
│   ├── System/                       # 新建——C++ Systems 目录
│   │   ├── MovementSystem.cpp        # Position += Velocity * dt
│   │   ├── RecalcStatsSystem.cpp     # HealthModifier → EnTT Health
│   │   ├── AOISystem.cpp             # 空间索引更新
│   │   └── ReplicateSystem.cpp       # 网络复制
│   ├── WorldServer.h                 # 修改——增加 DasLang Context 成员
│   ├── WorldServer.cpp
│   ├── LogicThread.h                 # 保留——onTick 改为调脚本 update()
│   ├── LogicThread.cpp
│   └── Handler/                      # 逐步废弃——业务逻辑迁移到 Scripts/
│       ├── EnterWorldHandler.cpp
│       └── MoveHandler.cpp
│
└── Tests/                            # 新建——C++ 单元测试
    └── ECS/
        └── TestEntityManager.cpp
```

## 15. 实施路线（4 阶段）

### Phase 1：DasLang 编译链路 + DECS 验证 + 基准测试（~2-3 天）

- **1.0** `Src/World/xmake.lua` 追加 `add_deps("libDaScript")` 和 DECS header 搜索路径
- **1.1** 验证 `xmake build WorldServer` 完整编译通过（daScript 全链路）
  - 实测确认 daslib includes 路径（`ThirdParty/daScript/daslib/`）在运行时可以被 `require` 解析
- **1.2** 在 `WorldServer.h` 中添加 `das::ContextPtr` / `das::ProgramPtr` 成员
- **1.3** 在 `WorldServer::Init()` 中加入最小 DasLang 初始化：
  - `compileDaScript("Scripts/ServerTick.das")` → `simulate()` → `restart()`
  - 调用脚本 `init()` 和 `update()`
  - 目标：一条 `Log::Info("script: hello")` 出现在日志中
- **1.4** 创建 `Scripts/ServerTick.das`，内嵌最简单的 DECS 验证：
  ```das
  require daslib/decs_boost
  require massive

  [export]
  def init() { massive_log_info("init: DECS ready") }

  [export]
  def update(sceneId: uint32; dt: float) {
      // DECS 最小 smoke test
      let e = decs::create_entity()
      create_entity() @(eid2, cmp) { cmp.eid := e }
      massive_log_info("update: entity created")
  }
  ```
- **1.5** **DECS query() 基准测试**：100 / 500 / 1000 entity 遍历耗时
  - 目标：确认 DECS 在目标量级下的性能是否满足 20ms Tick 预算
  - 不通过则评估：回退到自造 ECS API 或将部分系统移到 C++

> **修正**（2026-07-15）：原版 Phase 1 包含 "git submodule add daslib" 和 "编译 DECS 源文件"，这些步骤不存在——DECS 是纯 .das 脚本，已在 `ThirdParty/daScript/daslib/` 中。实测算工作量约 2-3 天而非原估 3-5 天。

### Phase 2：Bridge 窄接口 + EntityManager + GenMsgBindings（~4-6 天）

- **2.1** 实现 `MassiveModule` 中的核心函数：`create_entity`, `entity_position`, `entity_get_battlestats`, entity tag 判断（`is_dead` / `is_in_combat` / `is_stunned` / `is_player` / `is_monster`）, `send_to_client`, `log_info`
- **2.2** 实现 `EntityManager` 统一 EnTT/DECS entity 创建（含 generation 同步）
  - **重点测试**：创建-销毁-复用场景（§6.4 描述的 ID 复用边界条件）
- **2.3** 实现 `schedule_timer` + `cancel_timer`（桥接 C++ TimingWheel 到 DasLang block 回调）
- **2.4** **`GenMsgBindings.py`** 代码生成器（扫描 `.proto` → 产出 `MsgDispatch.gen.cpp` + `MsgArgs.gen.h` + `HandlerRegistry.das` + `MsgIDConstants.das`）
  - xmake `gen_msg_bindings` rule 集成
  - 入站消息自动生成 C++ switch case + DasLang Args struct
  - 出站消息自动生成 `massive_build_{msg_name}` C++ 函数
- **2.5** 脚本侧写一组测试 entity + DECS query，验证 Bridge 双向通信
- **2.6** 编写 `TestEntityManager.cpp` 单元测试（覆盖创建/销毁/复用/跨 EnTT-DECS 一致性）

### Phase 3：消息迁移到脚本（~2-3 天）

- **3.1** 定义 `Components.das` 中的 DECS template（EntityRef / PlayerData / MoveIntent，参见 §10.7）
- **3.2** `Handlers.das` 中实现 `EnterWorld` 和 `Move` 的业务逻辑（用 `[msg_handler]` 注解——Phase 2 已产出 HandlerRegistry）
- **3.3** 验证：`xmake up` → TestClient 登录 → 移动 → 日志正确
- **3.4** 保留 C++ Handler 作为 fallback（feature flag `_useScriptHandlers`）

### Phase 4：业务系统 + 热更新（后续迭代）

- **4.1** Buff 系统（DoT/HoT tick + 过期移除）
- **4.2** 技能 CD 系统
- **4.3** AI 行为树节点（DasLang 实现 + C++ 调度器）
- **4.4** 热更新集成（`ReloadScript()` + `decs_live`）
- **4.5** GM 脚本重载指令

## 16. 风险与缓解

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| DECS 性能不满足 1000 entity 遍历 | 低 | 中 | Phase 1.5 基准测试提前验证；不通过则回退到自造 ECS API 或将对应系统移到 C++ |
| DasLang xmake 编译在 Windows 上遇到问题 | 低 | 低 | `daslang.lua` 已有 MSVC 兼容配置（`/bigobj` `/EHa`），Phase 1.1 验证即可 |
| DECS 与 EnTT entity ID 同步 bug | 中 | 中 | EntityManager 是唯一入口，生命周期统一管理；Phase 2 重点测试 ID 复用场景 |
| 脚本编译错误导致服务器崩溃 | 低 | 中 | context 级别的 try/catch + 编译错误不回滚旧代码；`program->failed()` 检查 |
| 热更新时 DECS 反序列化失败 | 低 | 低 | `decs_live` 有 try/recover 保护；兼容性由 Archive 保证 |
| Protobuf 预解析的 switch case 膨胀 | 低 | 低 | `GenMsgBindings.py` Phase 2 即产出——零手工维护 |

## 17. 决策汇总

| 维度 | 选择 | 反对的方案 | 理由 |
|------|------|-----------|------|
| 脚本 ECS | **DECS**（daslib/decs + decs_boost） | 自定义 ecs_query/ecs_edit/ecs_stage | DECS 已有 27 个测试 + 序列化 + 热更新 + 调试，自造成本 ~2000 行 vs 复用 ~400 行 |
| C++ ECS | **保留 EnTT** | 全部迁移到 DECS | Position/Velocity/Collider 的高频遍历需要 EnTT SoA + SIMD 性能 |
| Bridge 接口 | **18 个窄函数（编译期绑定）** | 通用 ECS 查询系统（per-component getter/setter）/ 字符串 Tag 查询（strcmp 运行时开销） | 窄接口足够覆盖脚本所有跨边界需求；独立布尔函数方案消除 strcmp + 提供编译期错误检查 |
| 双缓冲 | **不做** | 原案的双缓冲 current/next swap | 每份数据只有一个 writer，不存在冲突；Stage 顺序正确反映数据依赖即可 |
| 线程模型 | **LogicThread 独占** | 脚本线程池 | 与现有 LogicThread 架构一致，无并发问题 |
| Protobuf 反序列化 | **C++ 预解析 + 原生类型投递** | 脚本内解析 Protobuf | 脚本零 Protobuf 依赖；C++ 侧类型安全；符合"Bridge 做窄接口"原则 |
| 消息处理 | **C++ switch case 分派到脚本 handler** | 保留 C++ MessageDispatcher 直派 | 业务 Handler 可热更；C++ 控制消息保留 C++ 处理 |
| 热更新 | **Context::restart + relocateCode** | 重启进程 | DasLang 原生能力；DECS `decs_live` 一行 require 搞定状态持久化 |
| 错误处理 | **编译失败/panic → 日志 + fallback** | 异常传播到 C++ 层 | 脚本错误不应导致服务器崩溃；热重载失败保留旧代码 |
