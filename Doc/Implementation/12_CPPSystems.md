# 脚本引擎 #12：CPPSystems — 物理模拟与 C++ 后台系统

> 状态：**设计中**（2026-07-15）
> 关联：[09_ScriptEngine](09_ScriptEngine.md)（总纲 §8）、[11_EntityManager](11_EntityManager.md)（EnTT Component 定义）
> 前置依赖：Position/Health/BattleStats 等 C++ Component 已定义、EntityManager 和 MassiveModule 可用
> 对应 Phase：Phase 2（CPPSystems 骨架） + Phase 3（RecalcStats 完整实现）

## 1. 定位

CPPSystems 是 LogicThread 中**在脚本 Tick 之后运行的 C++ 后台系统**。它们操作 EnTT Component——物理模拟、属性汇总、空间索引更新、网络复制。这些系统不访问 DECS（或仅通过 `ScriptBridge` 读取 DECS 组件值）。

## 2. 架构位置

```
LogicThread::RunLoop 单次迭代：
  Phase 2: ProcessMessages（消息分发 → 部分消息触发脚本 handler）
  Phase 3: TimingWheel.Tick（定时器回调——可能在 DECS 中写数据）
  Phase 5: onTick(budget) → WorldServer::OnTick()
             ├── ProcessUnroutedMessages()
             ├── ProcessControlMessages()
             ├── UpdateLoadLevel()
             ├── das_invoke(update)         ← 脚本 Tick（DECS stages）
             └── RunCPPSystems(scene, dt)   ← CPPSystems（本节）
  Phase 6: postFlush()
```

**执行顺序约束**：
1. 脚本 Tick 的 `update()` 中执行所有 `decs_stage()`——DECS 数据在脚本侧完成本帧修改
2. `RunCPPSystems()` 在脚本之后运行——此时可从 DECS 读取数据（如 HealthModifier），可安全写 EnTT
3. 脚本 Tick 和 CPPSystems **在同一线程**中顺序执行——无并发问题

## 3. 系统清单

| 系统 | 操作 | EnTT 读 | EnTT 写 | DECS 读 | Phase |
|------|------|---------|---------|---------|-------|
| **MovementSystem** | Position += Velocity × dt | Position, Velocity | Position | — | Phase 2 |
| **RecalcStatsSystem** | HealthModifier → Health | Health, BattleStats | Health | HealthModifier | Phase 3 |
| **AOISystem** | 空间索引更新 | Position | —（索引内部） | — | Phase 4 |
| **ReplicateSystem** | 差量序列化 | Position, Health, … | — | — | Phase 4 |
| **CombatTimeoutSystem** | CombatTag 过期检查 | CombatTag | CombatTag remove | — | Phase 3 |

## 4. MovementSystem（Phase 2 完整实现）

```cpp
// Src/World/System/MovementSystem.cpp
#include "Common/ECS/Scene.h"
#include "Common/ECS/Position.h"
#include "Common/ECS/Tags.h"

namespace MMO
{

/**
 * @brief Position += Velocity × dt
 *
 * 遍历所有有 Position + Velocity 组件的 entity（不含 DeadTag）。
 * 最简单的物理模拟——只做线性位移。
 *
 * 复杂度：O(N)，N = 有 Position+Velocity 的 entity 数
 */
void SystemMovement(ECS::Scene &scene, float dt)
{
    auto view = scene.Registry().view<Position, Velocity>(
        entt::exclude<DeadTag>);

    for (auto [e, pos, vel] : view.each())
    {
        pos.x += vel.vx * dt;
        pos.y += vel.vy * dt;
        pos.z += vel.vz * dt;

        // 标记脏数据——网络复制用
        scene.MarkDirty<Position>(Entity{scene.SceneID(),
                                         static_cast<uint32_t>(entt::to_integral(e))});
    }
}

} // namespace MMO
```

> **注意**：没有碰撞检测、没有 NavMesh 约束——这些是 Phase 4+ 的内容。
> Phase 2/3 的移动只做最简单的线性位移，服务器权威校验在脚本 handler 中通过速度上限检查完成。

### 4.1 Velocity 组件定义

```cpp
// Src/Common/ECS/Velocity.h
struct Velocity {
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;
};
```

## 5. RecalcStatsSystem（Phase 3 完整实现）

```cpp
// Src/World/System/RecalcStatsSystem.cpp
#include "Common/ECS/Scene.h"
#include "Common/ECS/Health.h"
#include "Common/ECS/BattleStats.h"
#include "Common/ECS/ScriptBridge.h"

namespace MMO
{

/**
 * @brief 汇总 DECS HealthModifier → 应用到 EnTT Health
 *
 * 通过 ScriptBridge 读取 DECS 中的 HealthModifier 组件，
 * 汇总所有 modifier 后一次性应用到 EnTT Health.current。
 *
 * 复杂度：O(M)，M = 有 HealthModifier 的 entity 数（≤ 战斗中的实体数）
 */
void SystemRecalcStats(ECS::Scene &scene, ScriptBridge &bridge)
{
    auto &registry = scene.Registry();

    // 1. 获取所有有 HealthModifier 的 entity
    auto modifiedEids = bridge.GetEntitiesWithComponent("HealthModifier");

    for (uint32_t eid : modifiedEids)
    {
        entt::entity e{eid};

        // 只处理 EnTT 中有效的 entity
        if (!registry.valid(e) || !registry.all_of<Health>(e))
            continue;

        // 2. 读取 DECS 中的 modifier
        auto modifier = bridge.GetComponentValue<HealthModifier>(eid, "HealthModifier");
        if (!modifier) continue;

        // 3. 应用到 EnTT Health
        auto &health = registry.get<Health>(e);

        // flatDelta 直接加减
        health.current += modifier->flatDelta;

        // pctDelta 是万分比：pctDelta=500 → +5%
        if (modifier->pctDelta != 0)
        {
            int32_t pctEffect = static_cast<int32_t>(
                static_cast<int64_t>(health.max) * modifier->pctDelta / 10000);
            health.current += pctEffect;
        }

        // 限幅
        if (health.current > health.max)  health.current = health.max;
        if (health.current < 0)           health.current = 0;

        // 4. 清空 modifier（下 Tick 重新累积）
        bridge.ClearComponent(eid, "HealthModifier");

        // 5. 标记脏数据
        scene.MarkDirty<Health>(Entity{scene.SceneID(), eid});
    }
}

} // namespace MMO
```

### 5.1 HealthModifier DECS 定义

```das
// Scripts/Components.das
[decs_template]
struct HealthModifier {
    flatDelta : int       // 加法修正（伤害→负数，治疗→正数）
    pctDelta  : int       // 百分比修正（万分比）
}
```

## 6. CombatTimeoutSystem（Phase 3）

```cpp
// Src/World/System/CombatTimeoutSystem.cpp

void SystemCombatTimeout(ECS::Scene &scene, float tickTime, float dt)
{
    auto view = scene.Registry().view<CombatTag, LastCombatTime>();

    for (auto [e, tag, lastTime] : view.each())
    {
        // 5 秒无战斗事件 → 退出战斗
        if (tickTime - lastTime.time > 5.0f)
        {
            scene.Registry().remove<CombatTag>(e);

            uint32_t eid = static_cast<uint32_t>(entt::to_integral(e));
            scene.MarkDirty<CombatTag>(Entity{scene.SceneID(), eid});

            Log::Debug("CombatTimeout: entity {} left combat", eid);
        }
    }
}

// 辅助组件
struct LastCombatTime {
    float time = 0.0f;  // 最后一次战斗事件的时间戳
};
```

## 7. RunCPPSystems 调度

```cpp
// WorldServer::OnTick() 中——脚本 Tick 之后

void WorldServer::RunCPPSystems(ECS::Scene &scene, float dt, float tickTime)
{
    MASSIVE_PROFILE_NAME("CPPSystems");

    // 执行顺序：Movement → RecalcStats → CombatTimeout
    // (Phase 4 加入 AOISystem / ReplicateSystem)

    SystemMovement(scene, dt);
    SystemRecalcStats(scene, *_scriptBridge);
    SystemCombatTimeout(scene, tickTime, dt);
}
```

**排序理由**：
- **Movement 先**——更新位置后，combat 的 AI 决策才能基于正确位置
- **RecalcStats 次**——HealthModifier 汇总必须发生在所有脚本 Stage 完成后
- **CombatTimeout 最后**——检查是否需要移除 CombatTag

## 8. 文件清单

```
Src/Common/ECS/
├── Position.h & Velocity.h        # 新建——物理层基础组件
├── Health.h                        # 新建——生命值组件
├── BattleStats.h                   # 新建——战斗属性组件
├── Tags.h                          # 新建——标签组件（DeadTag/CombatTag/...）
├── LastCombatTime.h                # 新建——最后战斗时间戳

Src/World/System/
├── MovementSystem.cpp              # 新建——Position += Velocity × dt
├── RecalcStatsSystem.cpp           # 新建——HealthModifier → Health
├── CombatTimeoutSystem.cpp         # 新建——战斗状态过期检查
├── (AOISystem.cpp)                 # Phase 4
└── (ReplicateSystem.cpp)           # Phase 4

Src/Common/ECS/
└── ScriptBridge.h / .cpp           # Phase 3——C++ 读取 DECS 状态

Scripts/
└── Components.das                  # 追加 HealthModifier template
```

## 9. 依赖

| 依赖 | 状态 |
|------|------|
| Position/Velocity/Health/BattleStats 定义 | Phase 2 产出 |
| EntityManager | Phase 2 产出 |
| MassiveModule（MovementSystem 不依赖 Bridge，独立于脚本层） | Phase 2 产出 |
| ScriptBridge（RecalcStats 需要读 DECS） | Phase 3 产出 |
| HealthModifier DECS template | Phase 3 产出 |
