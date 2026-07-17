# 脚本引擎 #11：EntityManager — EnTT/DECS 统一实体生命周期

> 状态：**设计中**（2026-07-15）
> 关联：[09_ScriptEngine](09_ScriptEngine.md)（总纲 §6）、[10_BridgeModule](10_BridgeModule.md)（MassiveModule）
> 前置依赖：Phase 1（DasLang Context + DECS 验证完成）
> 对应 Phase：Phase 2（EntityManager）

## 1. 定位

`EntityManager` 是 EnTT（C++ 物理层）和 DECS（脚本逻辑层）之间的**实体生命周期同步器**。

核心保证：两个独立的 ECS 系统共享**同一个 entity ID + generation 空间**，创建/销毁操作原子执行，不对存在"EnTT 中有但 DECS 中无"的中间态。

## 2. 核心原理

```
           EntityManager（唯一入口）
                  │
      ┌───────────┼───────────┐
      ▼                       ▼
  EnTT::create()         decs::create_entity()
  (物理层权威)            (脚本层同步——同一 ID)
      │                       │
      └───────────┬───────────┘
                  ▼
         返回 uint64(sceneID << 32 | entityID)
```

**关键约束**：
- Entity 创建/销毁**必须**通过 `EntityManager`——不能绕过它直接调 `Scene::CreateEntity()` 或 `decs::create_entity()`
- EnTT 是 ID 的**权威分配者**——DECS 使用与 EnTT 相同的 `entityID` 和 `generation`
- EntityManager 维护轻量的 `entityID → generation` 映射表（在 EnTT 的 `on_construct` / `on_destroy` 回调中更新）

## 3. 接口定义

```cpp
// Src/Common/ECS/EntityManager.h
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <entt/entt.hpp>

namespace das { class Context; }

namespace MMO::ECS
{
    class Scene;

    enum class EntityType : int32_t
    {
        Player  = 0,
        NPC     = 1,
        Monster = 2,
        Pet     = 3,
    };

    /**
     * @brief 统一 Entity 生命周期管理器
     *
     * 保证 EnTT 和 DECS 中的 entity ID + generation 完全同步。
     * 对外暴露 uint64 = (sceneID << 32) | entityID。
     */
    class EntityManager
    {
    public:
        explicit EntityManager(das::Context *dasCtx);

        // ── 生命周期（唯一入口）──

        /**
         * @brief 创建实体——同时分配 EnTT + DECS ID
         * @param scene    目标场景
         * @param pos      出生位置
         * @param type     实体类型
         * @return uint64 fullEntityId；0 表示失败
         */
        uint64_t CreateEntity(Scene &scene, float x, float y, float z, EntityType type);

        /**
         * @brief 销毁实体——先清理 DECS 组件，再销毁 EnTT entity
         * @param scene         目标场景
         * @param fullEntityId  完整 entity ID
         */
        void DestroyEntity(Scene &scene, uint64_t fullEntityId);

        // ── Entity ID 工具 ──

        /**
         * @brief 从 fullEntityId 提取 entityID
         */
        static uint32_t ExtractEntityID(uint64_t fullEntityId)
        {
            return static_cast<uint32_t>(fullEntityId & 0xFFFFFFFF);
        }

        /**
         * @brief 从 fullEntityId 提取 sceneID
         */
        static uint32_t ExtractSceneID(uint64_t fullEntityId)
        {
            return static_cast<uint32_t>(fullEntityId >> 32);
        }

        /**
         * @brief 组装 fullEntityId
         */
        static uint64_t MakeFullID(uint32_t sceneID, uint32_t entityID)
        {
            return (static_cast<uint64_t>(sceneID) << 32) | entityID;
        }

        // ── Generation 查询 ──

        /**
         * @brief 获取指定 entityID 的 generation（用于 DECS EntityId 构造）
         * @return generation；entity 不存在返回 0
         */
        uint32_t GetGeneration(uint32_t entityID) const;

        // ── Session 映射 ──

        /**
         * @brief 获取 entity 对应的 sessionID（如果有）
         * @return sessionID；entity 没有 session 返回 nullptr
         */
        const uint32_t *GetSessionIDForEntity(uint32_t entityID) const;

        /**
         * @brief 绑定 sessionID ↔ entityID
         */
        void BindSession(uint32_t sessionID, uint32_t entityID);

        /**
         * @brief 解除绑定
         */
        void UnbindSession(uint32_t entityID);

    private:
        // DECS 侧创建/销毁 — 调 DasLang 函数
        void CreateInDECS(uint32_t entityID, uint32_t generation, EntityType type);
        void DestroyInDECS(uint32_t entityID);

        das::Context *_dasCtx;

        // entityID → generation 映射
        std::unordered_map<uint32_t, uint32_t> _generations;

        // entityID → sessionID（仅 Player 使用）
        std::unordered_map<uint32_t, uint32_t> _entityToSession;
    };

} // namespace MMO::ECS
```

## 4. 创建流程

```cpp
uint64_t EntityManager::CreateEntity(Scene &scene, float x, float y, float z, EntityType type)
{
    // 1. EnTT 创建（权威 ID 分配）
    entt::entity e = scene.Registry().create();
    uint32_t entityID   = static_cast<uint32_t>(entt::to_integral(e));
    uint32_t generation = scene.Registry().current(e);  // EnTT version

    // 2. 记录 generation（后续 DECS 构造 EntityId 需要）
    _generations[entityID] = generation;

    // 3. Emplace EnTT 组件
    Entity we{scene.SceneID(), entityID};
    scene.EmplaceComponent<Position>(we, x, y, z);
    scene.EmplaceComponent<EntityTypeTag>(we, type);

    // 4. DECS 创建（同一 ID + generation）
    CreateInDECS(entityID, generation, type);

    // 5. 返回统一 ID
    return MakeFullID(scene.SceneID(), entityID);
}
```

### 4.1 DECS 侧创建

```cpp
void EntityManager::CreateInDECS(uint32_t entityID, uint32_t generation, EntityType type)
{
    // 调 DasLang 函数 decs_create_synced
    // 脚本侧实现：
    //   [export] def decs_create_synced(eid : uint32; gen : uint32; etype : int) {
    //       let e = create_entity(eid = EntityId{id = eid; generation = gen})
    //       create_entity() @(eid2, cmp) {
    //           cmp.eid := MakeFullID(sceneID, eid)
    //           cmp.entityType := etype
    //       }
    //   }

    auto fn = _dasCtx->findFunction("decs_create_synced");
    if (!fn)
    {
        Log::Warn("EntityManager: decs_create_synced not found in script");
        return;
    }

    das_invoke<void>::invoke(_dasCtx, nullptr, fn, entityID, generation, static_cast<int32_t>(type));
}
```

## 5. 销毁流程

```cpp
void EntityManager::DestroyEntity(Scene &scene, uint64_t fullEntityId)
{
    uint32_t entityID = ExtractEntityID(fullEntityId);

    // 1. DECS 清理（先清理脚本侧组件）
    DestroyInDECS(entityID);

    // 2. EnTT 销毁
    scene.DestroyEntity(Entity{ExtractSceneID(fullEntityId), entityID});

    // 3. 清理映射表
    _generations.erase(entityID);
    UnbindSession(entityID);
}

void EntityManager::DestroyInDECS(uint32_t entityID)
{
    auto fn = _dasCtx->findFunction("decs_destroy_synced");
    if (fn)
        das_invoke<void>::invoke(_dasCtx, nullptr, fn, entityID);
}
```

## 6. ID 复用边界场景（测试重点）

Entity 被销毁 → EnTT 可能复用同一个 entity ID（下一次 `create()`），但 **generation/version 会递增**。取决于 `entt::registry` 的实现：

| 场景 | EnTT 行为 | DECS 预期行为 |
|------|----------|--------------|
| 创建 entity A (id=1, gen=1) | entity_id 分配到 1，版本 1 | EntityId{1, 1} 绑定到 A |
| 销毁 entity A | entity_id 1 进入 free-list | old EntityId{1,1} 失效 |
| 创建 entity B | 可能复用 id=1，版本 → 2 | EntityId{1, 2} 绑定到 B |
| 访问 EntityId{1, 1} | `valid(e)` = false | `is_alive()` = false |

**关键验证**：
1. 创建-销毁-复用后，旧 ID 在 DECS 中 `is_alive()` 返回 false
2. 新 entity B 在 DECS 中不残留 entity A 的任何组件
3. EnTT 的 `registry.valid()` 和 DECS 的 `is_alive()` 对每个 entity 结果一致

## 7. Session 绑定

```cpp
void EntityManager::BindSession(uint32_t sessionID, uint32_t entityID)
{
    _entityToSession[entityID] = sessionID;
}

void EntityManager::UnbindSession(uint32_t entityID)
{
    _entityToSession.erase(entityID);
}

const uint32_t *EntityManager::GetSessionIDForEntity(uint32_t entityID) const
{
    auto it = _entityToSession.find(entityID);
    return (it != _entityToSession.end()) ? &it->second : nullptr;
}
```

Session 绑定的时机：
- **登录时**：`HandleEnterWorld` 完成后，C++ 侧调用 `BindSession(sessionID, entityID)`
- **断开时**：`OnDisconnectTimeout` 中销毁 entity 后自动 `UnbindSession`

## 8. 文件清单

```
Src/Common/ECS/
├── EntityManager.h                # 新建——EntityManager 类定义
├── EntityManager.cpp              # 新建——CreateEntity/DestroyEntity/ID 映射
├── Position.h                     # 新建——struct Position { float x, y, z; }
├── Health.h                       # 新建——struct Health { int32_t current, max; }
├── BattleStats.h                  # 新建——struct BattleStats { ... }
├── Tags.h                         # 新建——EntityTypeTag / DeadTag / CombatTag / ...
└── MassiveModule.h / .cpp         # 新建——Bridge 函数实现（依赖 EntityManager）

Scripts/
├── ServerTick.das                 # 新建——顶级 Tick 调度 + init/shutdown
├── Components.das                 # 新建——DECS template 定义
└── DecsBridge.das                 # 新建——decs_create_synced / decs_destroy_synced
```

## 9. 脚本侧 DECS Bridge

```das
// Scripts/DecsBridge.das
// EntityManager 调用的 DECS 创建/销毁同步函数

require daslib/decs_boost
require massive

// DECS 中统一 entity 标识
[decs_template]
struct EntityRef {
    eid        : uint64     // (sceneID << 32) | entityID
    entityType : int        // EntityType enum
}

// C++ EntityManager 调用——在 DECS 中创建与 EnTT 相同 ID 的 entity
[export]
def decs_create_synced(eid : uint32; gen : uint32; etype : int)
{
    let decsEntity = create_entity(eid = EntityId{id = eid; generation = gen})
    create_entity() @(eid2, cmp) {
        cmp.eid := eid
        cmp.entityType := etype
    }
    commit()
}

// C++ EntityManager 调用——清理 DECS 侧所有组件
[export]
def decs_destroy_synced(eid : uint32)
{
    destroy_entity(EntityId{id = eid; generation = get_entity_generation(eid)})
    commit()
}
```

> **注意**：`decs_destroy_synced` 中 `get_entity_generation(eid)` 的具体实现需要在 Phase 1 中验证 DECS 是否提供"按 id 查当前 generation"的 API。如果没有，改用 `is_alive()` 检查 + try-destroy。

## 10. C++ Component 定义（Phase 2 一同产出）

这些 Component 是 EnTT 物理层的基础，Phase 2 需要先定义才能写 Bridge 函数：

```cpp
// Src/Common/ECS/Position.h
struct Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Src/Common/ECS/Health.h
struct Health {
    int32_t current = 0;
    int32_t max     = 0;
};

// Src/Common/ECS/BattleStats.h
struct BattleStats {
    int32_t currentHp    = 0;
    int32_t maxHp        = 0;
    int32_t currentMp    = 0;
    int32_t maxMp        = 0;
    int32_t attack       = 0;
    int32_t defense      = 0;
    int32_t magicAttack  = 0;
    int32_t magicDefense = 0;
    int32_t critRate     = 0;
    int32_t critDamage   = 0;
    int32_t dodgeRate    = 0;
    int32_t hitRate      = 0;
    int32_t attackSpeed  = 0;
    int32_t moveSpeed    = 0;
};

// Src/Common/ECS/Tags.h
struct PlayerTag  {};
struct MonsterTag {};
struct NPCTag     {};
struct DeadTag    {};
struct CombatTag  {};
struct StunnedTag {};
```
