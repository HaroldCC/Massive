# 脚本引擎 #10：MassiveModule 桥接函数实现

> 状态：**设计中**（2026-07-15）
> 关联：[09_ScriptEngine](09_ScriptEngine.md)（总纲 §5）、[03_RPC](03_RPC.md)（内部 RPC）、[01_Network](01_Network.md)（TCP 连接层）
> 前置依赖：Phase 1（DasLang Context 初始化完成）
> 对应 Phase：Phase 2（Bridge 窄接口）

## 1. 定位

`MassiveModule` 是 DasLang C++ Module——脚本与 C++ 世界的**唯一通道**。共 18 个函数，分 6 组：空间查询、属性查询、Tag/State 判断、世界交互、定时器、工具。

本文定义每个函数的 C++ 实现细节、DasLang 类型映射、错误处理策略。是对 `09_ScriptEngine.md` §5 的完整展开。

## 2. 类型映射参考

| DasLang 类型 | C++ 绑定类型 | Module 注册方式 |
|-------------|-------------|---------------|
| `float3` | `das::float3` | 内建——无需注册 |
| `uint32` | `uint32_t` | 内建 |
| `uint64` | `uint64_t` | 内建 |
| `int32` | `int32_t` | 内建 |
| `float` | `float` | 内建 |
| `bool` | `bool` | 内建 |
| `string` | `const char*`(input) / `das::string`(output) | 内建 |
| `array<uint64>` | `const das::TArray<uint64_t>&` | `addExtern` 自动推导 |
| `array<uint8>` | `const das::TArray<uint8_t>&` | `addExtern` 自动推导 |
| `block<(uint32):void>` | `const das::TBlock<void, uint32_t>&` | `addExtern` 自动推导 |

> **关键约定**：`DAS_BIND_FUN` 宏自动处理参数类型推导和返回值包装。`addBuiltInModule()` 已注册所有内建类型。

## 3. 函数清单与实现设计

### 3.1 空间查询（AOI / 范围技能 / 追击）

#### `massive_entity_position` — 读 EnTT Position

```cpp
// DasLang 签名: float3 massive_entity_position(uint64 fullEntityId)
// C++ 实现:
das::float3 massive_entity_position(uint64_t fullEntityId)
{
    uint32_t entityID = static_cast<uint32_t>(fullEntityId & 0xFFFFFFFF);
    uint32_t sceneID  = static_cast<uint32_t>(fullEntityId >> 32);

    auto *scene = _sceneMgr->GetScene(sceneID);
    if (!scene || !scene->IsValid(Entity{sceneID, entityID}))
        return das::float3();  // 哨兵：(0, 0, 0)

    auto &pos = scene->GetComponent<Position>(Entity{sceneID, entityID});
    return das::float3(pos.x, pos.y, pos.z);
}
```

**关键设计**：
- `fullEntityId = (sceneID << 32) | entityID` — 与现有 `Entity.h` 兼容
- entity 不存在时返回 `(0,0,0)`——脚本侧需检查返回值
- Position 目前未实现（在 `Src/World/System/` 为空），Phase 2 需先定义 `struct Position { float x, y, z; }`

#### `massive_entities_in_radius` — AOI 空间范围查询

```cpp
// DasLang: array<uint64> massive_entities_in_radius(float3 center, float radius)
// C++ 实现（Phase 2 使用 brute-force 遍历；Phase 4 后接入 AOI 空间索引）:
das::TArray<uint64_t> massive_entities_in_radius(const das::float3 &center, float radius)
{
    das::TArray<uint64_t> result(_ctx);  // _ctx 由 Module 保存在成员中

    // Phase 2 简化实现：遍历 Scene 中所有 entity 的 Position
    auto *scene = _sceneMgr->GetDefaultScene();
    if (!scene) return result;

    auto view = scene->Registry().view<Position>();
    for (auto [e, pos] : view.each())
    {
        float dx = pos.x - center.x;
        float dz = pos.z - center.z;
        if (dx * dx + dz * dz <= radius * radius)
        {
            uint32_t eid    = static_cast<uint32_t>(entt::to_integral(e));
            uint64_t fullID = (static_cast<uint64_t>(scene->SceneID()) << 32) | eid;
            result.push_back(fullID);
        }
    }
    return result;
}
```

> **注意**：遍历 EnTT view 中的 Position 组件。Position 是 EnTT 独占组件——这意味着脚本查询的范围只包含**有 Position 组件的 entity**（即物理实体），不包含纯 DECS entity。

### 3.2 属性查询——整体暴露 BattleStats

原案中的 `massive_entity_health(entityId)` + `massive_entity_stat(entityId, "attack")` 改为整体暴露 BattleStats struct。脚本一次调用拿全部属性，零字符串。

#### 注册 BattleStats 到 DasLang

```cpp
// MassiveModule 构造函数中——使用 ManagedStructureAnnotation 整体暴露
addAnnotation(new das::ManagedStructureAnnotation<BattleStats>("BattleStats", lib));
// → DasLang 侧自动生成 BattleStats 类型：
//   struct BattleStats {
//       currentHp, maxHp, currentMp, maxMp : int
//       attack, defense, magicAttack, magicDefense : int
//       critRate, critDamage, dodgeRate, hitRate : int
//       attackSpeed, moveSpeed : int
//   }
```

#### `massive_entity_get_battlestats` — 整体查询

```cpp
// DasLang: BattleStats? massive_entity_get_battlestats(uint64 entityId)
// 返回 optional——entity 不存在时返回 null
std::optional<BattleStats> massive_entity_get_battlestats(uint64_t fullEntityId)
{
    uint32_t entityID = static_cast<uint32_t>(fullEntityId & 0xFFFFFFFF);
    uint32_t sceneID  = static_cast<uint32_t>(fullEntityId >> 32);

    auto *scene = _sceneMgr->GetScene(sceneID);
    if (!scene || !scene->IsValid(Entity{sceneID, entityID}))
        return std::nullopt;

    auto e = entt::entity(entityID);
    if (!scene->Registry().all_of<BattleStats>(e))
        return std::nullopt;

    return scene->Registry().get<BattleStats>(e);
}
```

```das
// 脚本侧使用
let stats = massive_entity_get_battlestats(targetEid)
if stats != null {
    let dmg = calc_damage(100, stats.attack, stats.defense)
    massive_log_info("hp: {stats.currentHp}/{stats.maxHp}")
}
```

### 3.3 Tag/State 查询——独立布尔函数

每个 Tag 对应一个独立的 Bridge 函数——编译期绑定，零 `strcmp`，删除 Tag 时编译错误立即暴露。

```cpp
// ── 5 个核心 Tag ──
bool massive_entity_is_dead(uint64_t fullEntityId) {
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<DeadTag>(e);
}

bool massive_entity_is_in_combat(uint64_t fullEntityId) {
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<CombatTag>(e);
}

bool massive_entity_is_stunned(uint64_t fullEntityId) {
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<StunnedTag>(e);
}

bool massive_entity_is_player(uint64_t fullEntityId) {
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<PlayerTag>(e);
}

bool massive_entity_is_monster(uint64_t fullEntityId) {
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<MonsterTag>(e);
}

// ── 辅助函数──消除重复代码 ──
struct ResolvedEntity {
    ECS::Scene *scene;
    bool        valid;
    entt::entity e;
};

ResolvedEntity ResolveEntity(uint64_t fullEntityId) {
    uint32_t entityID = static_cast<uint32_t>(fullEntityId & 0xFFFFFFFF);
    uint32_t sceneID  = static_cast<uint32_t>(fullEntityId >> 32);
    auto *scene = _sceneMgr->GetScene(sceneID);
    if (!scene || !scene->IsValid(Entity{sceneID, entityID}))
        return {nullptr, false, entt::null};
    return {scene, true, entt::entity(entityID)};
}
```

```das
// 脚本侧使用
if massive_entity_is_dead(targetEid) {
    massive_log_warn("target is already dead")
    return
}

if massive_entity_is_stunned(casterEid) {
    send_skill_cast_rsp(sessionID, skillID, SKILL_ERR_CANT_CAST)
    return
}
```

**综合成本**：新增一个 Tag/State = 新增 1 个 3 行 C++ 函数 + 1 个 `addExtern` + 更新 `09_ScriptEngine §5.1`。

### 3.4 世界交互（创建/销毁/消息发送）

#### `massive_create_entity` — EnTT + DECS 双创建

```cpp
// DasLang: uint64 massive_create_entity(float3 pos, int32 entityType)
uint64_t massive_create_entity(const das::float3 &pos, int32_t entityType)
{
    // 1. EnTT 创建
    auto *scene = _sceneMgr->GetDefaultScene();
    if (!scene) return 0;

    entt::entity e      = scene->Registry().create();
    uint32_t     entityID = static_cast<uint32_t>(entt::to_integral(e));
    uint32_t     generation = scene->Registry().current(e);

    // 2. Emplace 基础组件
    Entity entity{scene->SceneID(), entityID};
    scene->EmplaceComponent<Position>(entity, pos.x, pos.y, pos.z);
    scene->EmplaceComponent<EntityTypeTag>(entity, static_cast<EntityType>(entityType));

    // 3. DECS 同步（通过 EntityManager——见 §11_EntityManager.md）
    _entityMgr->OnEnTTEntityCreated(entityID, generation, entityType);

    // 4. 返回统一 ID
    return (static_cast<uint64_t>(scene->SceneID()) << 32) | entityID;
}
```

#### `massive_destroy_entity`

```cpp
// DasLang: void massive_destroy_entity(uint64 fullEntityId)
void massive_destroy_entity(uint64_t fullEntityId)
{
    uint32_t entityID = static_cast<uint32_t>(fullEntityId & 0xFFFFFFFF);
    uint32_t sceneID  = static_cast<uint32_t>(fullEntityId >> 32);

    auto *scene = _sceneMgr->GetScene(sceneID);
    if (!scene) return;

    // 1. DECS 先清理（通过 EntityManager）
    _entityMgr->OnEntityDestroyed(entityID);

    // 2. EnTT 销毁
    scene->DestroyEntity(Entity{sceneID, entityID});
}
```

#### `massive_send_to_client` — 脚本出站

```cpp
// DasLang: void massive_send_to_client(uint32 sessionId, uint32 msgId, array<uint8> data)
void massive_send_to_client(uint32_t sessionID, uint32_t msgID,
                            const das::TArray<uint8_t> &data)
{
    // data 是 protobuf 序列化后的字节——C++ 不做任何解析
    // 通过 WorldServer 的 SendToClient 方法出站
    _worldServer->SendRawToClient(sessionID, msgID,
                                  data.data, static_cast<size_t>(data.size));
}
```

> **设计注意**：`SendRawToClient` 需要在 `WorldServer` 中新增一个公共方法，不经过 MessageDispatcher（因为 body 已经是 protobuf bytes）：
> ```cpp
> void WorldServer::SendRawToClient(uint32 sessionID, uint32 msgID,
>                                   const uint8 *data, size_t len);
> ```

#### `massive_broadcast_nearby` — 范围广播

```cpp
// DasLang: void massive_broadcast_nearby(float3 center, float radius, uint32 msgId, array<uint8> data)
void massive_broadcast_nearby(const das::float3 &center, float radius,
                              uint32_t msgID, const das::TArray<uint8_t> &data)
{
    // 1. 查范围内所有 entity
    auto entities = massive_entities_in_radius(center, radius);

    // 2. 对每个 entity 找到对应的 sessionID → 发送
    for (auto fullID : entities)
    {
        uint32_t eid = static_cast<uint32_t>(fullID & 0xFFFFFFFF);
        auto *sessionID = _entityMgr->GetSessionIDForEntity(eid);
        if (sessionID)
            _worldServer->SendRawToClient(*sessionID, msgID, data.data, data.size);
    }
}
```

### 3.5 定时器

#### `massive_schedule_timer`

```cpp
// DasLang: uint32 massive_schedule_timer(int32 delayMs, block<(timerId:uint32):void>)
uint32_t massive_schedule_timer(int32_t delayMs, const das::TBlock<void, uint32_t> &block)
{
    // 1. 分配 timerID
    uint32_t timerID = _nextTimerID.fetch_add(1, std::memory_order_relaxed);

    // 2. 保存闭包引用（延长生命周期——block 在 das::Context 的 GC 下）
    _timerCallbacks[timerID] = {block, _ctx};  // 持有 ctx 引用，防止 GC 回收

    // 3. 注册到 C++ TimingWheel
    //    TimingWheel::Schedule 的回调在 LogicThread 中执行（同步）
    _timingWheel->Schedule(
        std::chrono::milliseconds(delayMs),
        [this, timerID]() {
            auto it = _timerCallbacks.find(timerID);
            if (it != _timerCallbacks.end())
            {
                // 在 LogicThread 中执行 DasLang 闭包
                das_invoke<void>::invoke(
                    _ctx.get(), nullptr, it->second.block,
                    timerID);  // 传 timerID 给闭包
                _timerCallbacks.erase(it);  // 单次回调后移除
            }
        });

    return timerID;
}
```

**关键约束**：
- 回调在 LogicThread 的 TimingWheel::Tick（RunLoop Phase 3）执行
- DECS 数据在回调中可安全读写（单线程）
- `_timerCallbacks` 必须延长 `block` 的生命周期——直接存 `das::TBlock` 到 `std::unordered_map` 中，使其在 GC 扫描时可达

#### `massive_cancel_timer`

```cpp
// DasLang: void massive_cancel_timer(uint32 timerId)
void massive_cancel_timer(uint32_t timerID)
{
    _timerCallbacks.erase(timerID);
    _timingWheel->Cancel(timerID);
}
```

### 3.6 工具函数

#### `massive_log_info` / `massive_log_warn` / `massive_log_error`

```cpp
void massive_log_info(const char *msg)  { Log::Info("[script] {}", msg); }
void massive_log_warn(const char *msg)  { Log::Warn("[script] {}", msg); }
void massive_log_error(const char *msg) { Log::Error("[script] {}", msg); }
```

#### `massive_get_dt`

```cpp
// DasLang: float massive_get_dt()
// 返回固定 0.02f（20ms Tick），不是 budget
float massive_get_dt() { return 0.02f; }
```

#### `massive_find_entity_by_session` — Session → Entity 反向查找

```cpp
// DasLang: uint64 massive_find_entity_by_session(uint32 sessionID)
uint64_t massive_find_entity_by_session(uint32_t sessionID)
{
    // 查 WorldServer::_sessions
    auto it = _sessions->find(sessionID);
    if (it == _sessions->end()) return 0;

    auto &entity = it->second.entity;
    return (static_cast<uint64_t>(entity.sceneId) << 32) | entity.entityId;
}
```

## 4. MassiveModule 完整类定义

```cpp
// Src/Common/ECS/MassiveModule.h
#pragma once

#include <atomic>
#include <daScript/daScriptModule.h>
#include <daScript/simulate/simulate.h>

namespace MMO
{
    class WorldServer;
    class EntityManager;
    class SceneManager;
    class TimingWheel;

    class MassiveModule : public das::Module
    {
    public:
        MassiveModule(WorldServer     *worldServer,
                      EntityManager   *entityMgr,
                      SceneManager    *sceneMgr,
                      TimingWheel     *timingWheel,
                      std::unordered_map<uint32, WorldSession> *sessions);

        // ── Module 注册 ──
        // 在构造函数中通过 addExtern 注册所有 18 个函数
        // 见 09_ScriptEngine §5.1

    private:
        // ── 上下文指针（Bridge 函数需要）──
        WorldServer                                          *_worldServer;
        EntityManager                                        *_entityMgr;
        SceneManager                                         *_sceneMgr;
        TimingWheel                                          *_timingWheel;
        std::unordered_map<uint32, WorldSession>             *_sessions;

        // ── 定时器回调存储 ──
        struct TimerCallback { das::TBlock<void, uint32_t> block; das::ContextPtr ctx; };
        std::unordered_map<uint32_t, TimerCallback> _timerCallbacks;
        std::atomic<uint32_t> _nextTimerID{1};
    };
}
```

## 5. 错误处理约定

| Bridge 函数返回类型 | 错误情况 | 返回值 |
|-------------------|---------|--------|
| `uint64`（entity ID） | scene 不存在 / entity 无效 | 0 |
| `std::optional<BattleStats>` | entity 无效 / 无 BattleStats | `nullopt` |
| `float3`（position） | entity 无效 | (0, 0, 0) |
| `bool`（tag 查询） | entity 无效 | false |
| `array<>`（查询结果） | scene 不存在 | 空数组 |
| `void`（send_to_client 等）| — | —（内部 Log 警告） |

**脚本侧责任**：每次 Bridge 调用后检查哨兵值。

```das
let pos = massive_entity_position(targetEid)
if pos == float3(0, 0, 0) {
    massive_log_warn("entity position not found")
    return
}
```

## 6. 实现顺序（Phase 2）

| 步骤 | 函数 | 测试方式 |
|------|------|---------|
| 2.1.1 | `massive_log_info` | 脚本打印 → 日志可见 |
| 2.1.2 | `massive_get_dt` | 打印 dt 值验证 |
| 2.1.3 | `massive_create_entity` + `massive_entity_position` | 创建 entity → 读坐标 |
| 2.1.4 | `massive_destroy_entity` | 创建 → 销毁 → 验证 Position 不可读 |
| 2.1.5 | `massive_send_to_client` | 脚本发送测试消息到 TestClient |
| 2.1.6 | `massive_schedule_timer` | 注册 1s 定时器 → 日志输出 |
| 2.1.7 | `massive_entities_in_radius` | 创建 3 个 entity → 范围查询 |
| 2.1.8 | `massive_entity_get_battlestats` / tag 判断（`is_dead` / `is_stunned` 等） | Emplace BattleStats → 脚本读
| 2.1.9 | `massive_find_entity_by_session` | 登录后验证 session→entity 映射 |

## 7. 依赖

| 依赖 | 状态 |
|------|------|
| `das::Context` 初始化 | Phase 1 产出 |
| Position / Health / BattleStats Component 定义 | Phase 2 需先定义 C++ struct |
| EntityTypeTag / DeadTag 等 Tag 组件 | Phase 2 需先定义 |
| EntityManager | Phase 2 并行实现 |
| TimingWheel（已有） | ✅ 可复用 |
| WorldServer::SendRawToClient | Phase 2 新增 |
