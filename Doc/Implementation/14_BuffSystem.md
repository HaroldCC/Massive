# 脚本引擎 #14：Buff 系统——DECS Stage 实现

> 状态：**设计中**（2026-07-15）
> 关联：[09_ScriptEngine](09_ScriptEngine.md)（总纲 §4.3 HealthModifier）、[12_CPPSystems](12_CPPSystems.md)（RecalcStatsSystem）、[buff_system](../前期设计/ecs/buff_system.html)（上游设计）
> 前置依赖：Phase 2 完成、HealthModifier DECS template 可用
> 对应 Phase：Phase 4

## 1. 定位

Buff 系统是 DECS 侧的 **Stage 实现**——所有 Buff 逻辑在脚本中运行，通过 `ecs_stage("buff_tick")` 统一调度。C++ 侧只负责将 HealthModifier 汇总后应用到 EnTT Health（`RecalcStatsSystem`）。

设计复用了 `buff_system.html` 的核心概念（Modifier 修饰器、Stack/Refresh/Replace 叠加规则、互斥组），将其适配到 DECS 架构——不再使用原案的 `ecs_edit` / `ecs_query` API，改用 DECS 的 `query()` + `update_entity()`。

## 2. 数据结构（DECS Template）

```das
// Scripts/Components.das — Buff 相关定义

[decs_template]
struct StatModifier {
    stat  : int      // StatType enum
    op    : int      // ModOp enum: 0=ADD, 1=MUL, 2=SET_MIN, 3=SET_MAX
    value : int      // ADD→直接值；MUL→万分比（500=+5%）
}

[decs_template]
struct BuffInstance {
    buffID        : uint32       // Buff 模板 ID
    casterEntity  : uint64       // 施法者 fullEntityId
    startTime     : float        // 施加时间（服务器时间秒）
    duration      : float        // 持续时长（秒），0 = 永久
    remainingTime : float        // 剩余时间（秒）
    maxStacks     : int          // 最大层数
    currentStacks : int          // 当前层数
    period        : float        // 周期性触发间隔（秒），0 = 非周期
    timeSinceTick : float        // 距离上次周期触发的时间
}

[decs_template]
struct BuffList {
    buffs : array<BuffInstance>
    dirty : bool                 // Modifier 需要重新汇总
}

// C++ 侧读取的 HealthModifier——脚本写入，C++ 每 Tick 汇总
[decs_template]
struct HealthModifier {
    flatDelta : int    // 加法修正（+/- HP）
    pctDelta  : int    // 百分比修正（万分比）
}
```

**类型简化 vs 上游设计**：
- 上游 `buff_system.html` 的 `StatModifier` 包含 `StatType` enum、`ModOp` enum、`BuffTrigger` enum——Phase 4 简化到只做 `HealthModifier`（HP 加减），框架搭好后再扩展其它属性
- `BuffList.modifiers`（每个 Buff 的 Modifier 列表）推迟到扩展期——HealthModifier 足够覆盖 MVP

## 3. 施加 Buff

```das
// Scripts/Buff.das
require daslib/decs_boost
require massive

let BUFF_STACK_RULE_STACK    = 0  // 叠加层数 + 刷新持续时间
let BUFF_STACK_RULE_REFRESH  = 1  // 只刷新持续时间，不叠加
let BUFF_STACK_RULE_REPLACE  = 2  // 新覆盖旧

// ── 施加 Buff ──
def apply_buff(entityID : uint32; casterID : uint64; buffID : uint32; stacks : int) : bool
{
    // Phase 4: buffID → 从 ConfigTable 查模板（TODO: ConfigTable 脚本绑定）
    // 当前传 template_data 作为参数，或者硬编码几个测试 Buff

    let rule = BUFF_STACK_RULE_STACK  // 模板中读取
    let maxStacks = 3                 // 模板中读取
    let duration  = 10.0              // 模板中读取
    let period    = 2.0               // 模板中读取
    let flatVal   = -5                // 模板中读取——每 Tick 扣 5 HP 的 DoT

    var applied = false

    query(entityID) $(var bl : BuffList) {
        // 检查同 ID Buff 是否已存在
        for var i = 0; i < length(bl.buffs); i++ {
            if bl.buffs[i].buffID == buffID {
                if rule == BUFF_STACK_RULE_REPLACE {
                    // 替换——移除旧的
                    bl.buffs |> erase(i)
                    break
                } elif rule == BUFF_STACK_RULE_REFRESH {
                    // 刷新持续时间
                    bl.buffs[i].duration = duration
                    bl.buffs[i].remainingTime = duration
                    bl.buffs[i].startTime = massive_get_tick_time()
                    bl.dirty = true
                    applied = true
                    return true
                } else {  // STACK
                    // 叠加层数
                    bl.buffs[i].duration = duration
                    bl.buffs[i].remainingTime = duration
                    bl.buffs[i].currentStacks = min(bl.buffs[i].currentStacks + stacks, maxStacks)
                    bl.buffs[i].startTime = massive_get_tick_time()
                    bl.dirty = true
                    applied = true
                    return true
                }
            }
        }

        // 新 Buff
        let inst = BuffInstance {
            buffID = buffID, casterEntity = casterID,
            startTime = massive_get_tick_time(),
            duration = duration, remainingTime = duration,
            maxStacks = maxStacks, currentStacks = stacks,
            period = period, timeSinceTick = 0.0f
        }
        bl.buffs |> push(inst)
        bl.dirty = true
    }

    // 施加时效果——通过 HealthModifier 间接影响 HP
    collect_modifiers(entityID)

    return true
}
```

## 4. 每 Tick 更新——`ecs_stage("buff_tick")`

```das
// 每 Tick 执行——在 ServerTick.das 的 update() 中调度
[decs(stage = "buff_tick")]
def system_buff_tick(dt : float)
{
    let now = massive_get_tick_time()

    query() $(var bl : BuffList) {
        let entityID = get_eid()  // DECS 内置

        // 倒序遍历——支持安全的 erase
        for var i = length(bl.buffs) - 1; i >= 0; i-- {
            var inst = bl.buffs[i]

            // 1. 时长递减 + 过期移除
            if inst.duration > 0.0f {
                inst.remainingTime -= dt
                if inst.remainingTime <= 0.0f {
                    // remove callback（TODO: 按模板 on_remove_script）
                    bl.buffs |> erase(i)
                    bl.dirty = true
                    continue
                }
            }

            // 2. 周期性触发（DoT/HoT）
            if inst.period > 0.0f {
                inst.timeSinceTick += dt
                while inst.timeSinceTick >= inst.period {
                    inst.timeSinceTick -= inst.period
                    on_buff_tick(entityID, bl.buffs[i])
                }
            }
        }
    }

    // 汇总所有 Modify 到 HealthModifier
    collect_all_modifiers()
}
```

## 5. Modifier 汇总 → HealthModifier

```das
// 遍历所有带 BuffList 的 entity，汇总 modifier → 写入 HealthModifier
def collect_all_modifiers()
{
    query() $(var bl : BuffList) {
        if !bl.dirty { continue }

        let entityID = get_eid()
        var flatSum  = 0
        var pctSum   = 0

        for inst in bl.buffs {
            // 每层 × 效果值
            let tickValue = -5  // TODO: 从模板读取
            flatSum += tickValue * inst.currentStacks
        }

        // 写入 HealthModifier——C++ RecalcStats 每 Tick 读取
        query(entityID) $(var hm : HealthModifier) {
            hm.flatDelta = flatSum
            hm.pctDelta  = pctSum
        }

        bl.dirty = false
    }
}

def collect_modifiers(entityID : uint32)
{
    query(entityID) $(var bl : BuffList; var hm : HealthModifier) {
        var flatSum = 0
        for inst in bl.buffs {
            flatSum += (-5) * inst.currentStacks  // TODO: 模板读取
        }
        hm.flatDelta = flatSum
        bl.dirty = false
    }
}
```

## 6. 数据处理流向

```
Buff.DoT (每 2s 触发一次)
  │
  ▼
on_buff_tick(entityID, buffInstance)
  │
  ▼
update_entity(entityID)
  → BuffList.dirty = true
  │
  ▼
collect_all_modifiers()
  → 遍历 BuffList → 汇总 modifier
  → 写入 HealthModifier{flatDelta=-5, pctDelta=0}
  │
  ▼
DECS commit()（Tick 结束时）
  │
  ▼
CPPSystems::RecalcStatsSystem
  → ScriptBridge::GetComponentValue<HealthModifier>(eid, "HealthModifier")
  → EnTT Health.current += flatDelta
  → ScriptBridge::ClearComponent(eid, "HealthModifier")
  │
  ▼
Health.current 被扣 5 HP
→ CurrentHp ≤ 0 ? → DeadTag + on_entity_killed 事件
```

## 7. 文件清单

```
Scripts/
├── Buff.das                       # 新建——Buff 系统（DoT/HoT/过期/叠加）
├── Components.das                 # 追加 BuffInstance / BuffList / HealthModifier
└── ServerTick.das                 # 追加 ecs_stage("buff_tick") 调度

Src/World/System/
└── RecalcStatsSystem.cpp          # Phase 3 已实现——HealthModifier → Health
```

## 8. Phase 4 扩展点

| 功能 | Phase 4 状态 | 扩展内容 |
|------|-------------|---------|
| Buff 模板（ConfigTable） | 硬编码 buffID → 参数 | 脚本侧 ConfigTable 绑定——从 DB 读取模板数据 |
| 多属性 Modifier | 仅 Health | 扩展到 attack/defense/critRate 等 BattleStats 所有字段 |
| Buff 互斥组 | 未实现 | 施加前检查 exclusive_group |
| Buff 免疫 | 未实现 | 检查 immunity_tags |
| Buff 触发事件 | 仅 periodic | 支持 on_damage_dealt / on_kill 等事件触发 |
| Buff 脚本回调 | 未实现 | on_apply_script / on_tick_script / on_remove_script 动态回调 |
