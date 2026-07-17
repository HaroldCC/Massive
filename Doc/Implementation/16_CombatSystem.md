# 脚本引擎 #16：战斗系统——伤害管线 + 仇恨 + 死亡

> 状态：**设计中**（2026-07-15）
> 关联：[09_ScriptEngine](09_ScriptEngine.md)（总纲 §4.3 HealthModifier）、[12_CPPSystems](12_CPPSystems.md)（RecalcStatsSystem、CombatTimeoutSystem）、[14_BuffSystem](14_BuffSystem.md)（Buff Modifier 汇总）、[combat_system](../前期设计/ecs/combat_system.html)（上游设计）
> 前置依赖：Phase 3 消息迁移完成、BattleStats 组件已定义、RecalcStatsSystem 可用
> 对应 Phase：Phase 4

## 1. 定位

战斗系统是 MMO 服务器的核心热路径。要害决策：

| 组件 | 位置 | 理由 |
|------|------|------|
| **伤害管线** | C++ 静态函数 → 脚本通过 Bridge 调用 | 每 Tick 数百次数学计算——走 C++ 热路径，零堆分配 |
| **仇恨表** | DECS Component（`ThreatTable`） | 低频操作（怪物数量 × 每次伤害事件），复杂排序逻辑适合脚本 |
| **战斗状态机** | EnTT Tag（`CombatTag` / `DeadTag`） | C++ 能高效检查——每 Tick 遍历所有 entity 判断是否离开战斗 |
| **战斗浮字** | 脚本出站广播 | `massive_broadcast_nearby()` 范围广播 |

## 2. C++ 伤害管线（Bridge 绑定）

### 2.1 管线接口

```cpp
// Src/World/System/DamagePipeline.h
namespace MMO
{

struct DamageEvent {
    uint64_t attackerFullID;  // 攻击者
    uint64_t defenderFullID;  // 防御者
    uint32_t skillID;
    int32_t  baseDamage;
    enum Type { Physical = 0, Magical = 1, True = 2 } damageType;
};

struct DamageResult {
    bool    isHit;
    bool    isCrit;
    int32_t finalDamage;
    int32_t hpBefore;
    int32_t hpAfter;
    enum Outcome { Miss = 0, Hit = 1, Crit = 2, Kill = 3 } outcome;
};

/**
 * @brief 伤害管线——纯静态方法，零状态，零堆分配
 *
 * 6 阶段流水线：
 *   1. 命中判定（hitRate - dodgeRate）
 *   2. 暴击判定（简单随机——PRD 后续扩展）
 *   3. 基础伤害计算（护甲减伤曲线）
 *   4. 伤害修正（预留——BuffList 查询）
 *   5. 吸收/护盾（Phase 4 暂不实现）
 *   6. 应用伤害→返回 DamageResult
 */
class DamagePipeline {
public:
    static DamageResult Execute(const DamageEvent &event,
                                const BattleStats &atkStats,
                                const BattleStats &defStats);

private:
    static bool    CheckHit(const BattleStats &atk, const BattleStats &def);
    static bool    CheckCrit(const BattleStats &atk);
    static int32_t CalcRawDamage(int32_t base, bool isCrit,
                                 const BattleStats &atk,
                                 const BattleStats &def,
                                 DamageEvent::Type type);

    static constexpr int32_t kDefenseConstant = 5000;
};

} // namespace MMO
```

### 2.2 Bridge 函数注册

```cpp
// MassiveModule 中新增
addExtern<DAS_BIND_FUN(massive_execute_damage)>(...);
//   int32 massive_execute_damage(uint64 attackerFullID, uint64 defenderFullID,
//                                int32 baseDamage, int32 damageType)
//   → 内部：构造 DamageEvent → DamagePipeline::Execute → 应用到 EnTT Health
//   → 返回 finalDamage（负数表示 Miss）
```

```cpp
int32_t massive_execute_damage(uint64_t attackerFullID, uint64_t defenderFullID,
                               int32_t baseDamage, int32_t damageType)
{
    uint32_t attackerID = EntityManager::ExtractEntityID(attackerFullID);
    uint32_t defenderID = EntityManager::ExtractEntityID(defenderFullID);
    auto *scene = _sceneMgr->GetDefaultScene();
    if (!scene) return -1;

    auto atkE = entt::entity(attackerID);
    auto defE = entt::entity(defenderID);
    auto &reg = scene->Registry();

    if (!reg.valid(defE) || !reg.all_of<BattleStats>(defE))
        return -1;

    // 攻击者可能没有 BattleStats（如环境伤害）→ 用默认值
    BattleStats atkStats{};
    if (reg.valid(atkE) && reg.all_of<BattleStats>(atkE))
        atkStats = reg.get<BattleStats>(atkE);

    auto &defStats = reg.get<BattleStats>(defE);

    DamageEvent event{attackerFullID, defenderFullID, 0, baseDamage,
                      static_cast<DamageEvent::Type>(damageType)};

    auto result = DamagePipeline::Execute(event, atkStats, defStats);

    if (result.outcome == DamageResult::Miss)
        return -1;

    // 应用伤害到 EnTT Health
    auto &health = reg.get<Health>(defE);
    health.current = result.hpAfter;
    scene->MarkDirty<Health>(Entity{scene->SceneID(), defenderID});

    // 标记进入战斗
    if (!reg.all_of<CombatTag>(defE))
        reg.emplace<CombatTag>(defE);

    return result.finalDamage;
}
```

### 2.3 管线核心算法

```cpp
bool DamagePipeline::CheckHit(const BattleStats &atk, const BattleStats &def)
{
    // 命中率 = 攻击方 hitRate - 防御方 dodgeRate
    // hitRate 和 dodgeRate 都是万分比
    int32_t hitChance = atk.hitRate - def.dodgeRate;
    if (hitChance < 500) hitChance = 500;   // 保底 5% 命中
    if (hitChance > 9500) hitChance = 9500; // 封顶 95%

    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<int32_t> dist(0, 9999);
    return dist(rng) < hitChance;
}

bool DamagePipeline::CheckCrit(const BattleStats &atk)
{
    int32_t critChance = atk.critRate;
    if (critChance > 8000) critChance = 8000;  // 封顶 80%

    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<int32_t> dist(0, 9999);
    return dist(rng) < critChance;
}

int32_t DamagePipeline::CalcRawDamage(int32_t base, bool isCrit,
                                       const BattleStats &atk,
                                       const BattleStats &def,
                                       DamageEvent::Type type)
{
    // Phase 4 简化公式
    float raw = static_cast<float>(base);

    // 暴击倍率
    if (isCrit) {
        raw *= (1.0f + static_cast<float>(atk.critDamage) / 10000.0f);
    }

    // 护甲减伤曲线：damage = raw × (1 - defense / (defense + K))
    if (type == DamageEvent::Physical && def.defense > 0) {
        float reduction = static_cast<float>(def.defense) /
                         (static_cast<float>(def.defense) + kDefenseConstant);
        raw *= (1.0f - reduction);
    } else if (type == DamageEvent::Magical && def.magicDefense > 0) {
        float reduction = static_cast<float>(def.magicDefense) /
                         (static_cast<float>(def.magicDefense) + kDefenseConstant);
        raw *= (1.0f - reduction);
    }
    // True damage: 无视防御，不变

    return static_cast<int32_t>(raw);
}

DamageResult DamagePipeline::Execute(const DamageEvent &event,
                                      const BattleStats &atkStats,
                                      const BattleStats &defStats)
{
    DamageResult result{};
    result.hpBefore = defStats.currentHp;

    // 阶段 1: 命中
    if (!CheckHit(atkStats, defStats)) {
        result.outcome = DamageResult::Miss;
        result.hpAfter = defStats.currentHp;
        return result;
    }
    result.isHit = true;

    // 阶段 2: 暴击
    result.isCrit = CheckCrit(atkStats);

    // 阶段 3: 基础伤害
    result.finalDamage = CalcRawDamage(event.baseDamage, result.isCrit,
                                       atkStats, defStats, event.damageType);

    // 阶段 6: 应用
    result.hpAfter = defStats.currentHp - result.finalDamage;
    if (result.hpAfter < 0) result.hpAfter = 0;

    result.outcome = result.hpAfter <= 0 ? DamageResult::Kill
                   : result.isCrit      ? DamageResult::Crit
                   :                     DamageResult::Hit;

    return result;
}
```

## 3. 仇恨系统（DECS 侧）

```das
// Scripts/Combat.das
require daslib/decs_boost
require massive

[decs_template]
struct ThreatEntry {
    targetEid : uint64   // 攻击者 fullEntityId
    threat    : int64    // 累积威胁值
}

[decs_template]
struct ThreatTable {
    entries : array<ThreatEntry>
    dirty   : bool
}

// 添加威胁
def add_threat(monsterID : uint32; attackerFullID : uint64; amount : int64)
{
    query(monsterID) $(var tt : ThreatTable) {
        for var entry in tt.entries {
            if entry.targetEid == attackerFullID {
                entry.threat += amount
                tt.dirty = true
                return
            }
        }
        tt.entries |> push(ThreatEntry{targetEid = attackerFullID, threat = amount})
        tt.dirty = true
    }
}

// 获取仇恨最高目标
def get_top_threat_target(monsterID : uint32) : uint64
{
    var result = uint64(0)
    query(monsterID) $(var tt : ThreatTable) {
        if tt.dirty {
            // 按 threat 降序排序
            sort(tt.entries, $(a, b) => b.threat < a.threat)
            tt.dirty = false
        }
        if length(tt.entries) > 0 {
            result = tt.entries[0].targetEid
        }
    }
    return result
}

// 清空仇恨表（脱战/死亡时）
def clear_threat_table(monsterID : uint32)
{
    query(monsterID) $(var tt : ThreatTable) {
        tt.entries |> clear()
        tt.dirty = true
    }
}
```

### 3.1 威胁值规则

| 行为 | 威胁值 |
|------|--------|
| 造成 1 点伤害 | +1 威胁 |
| 治疗（有效治疗） | +0.5 威胁/每点 |
| 嘲讽技能 | 当前最高威胁 × 1.1 |
| 进入战斗初始威胁 | +10 |
| 超出追击范围 | 威胁清零（脱战） |

## 4. 战斗浮字广播

```das
// 伤害结算后——脚本广播战斗浮字
def broadcast_combat_float(targetFullID : uint64; value : int32;
                           floatType : int; skillID : uint32)
{
    let targetPos = massive_entity_position(targetFullID)

    // 构建 CombatFloatNtf protobuf body
    let bodyData = massive_build_combat_float(targetFullID, floatType,
                                               value, skillID)
    massive_broadcast_nearby(targetPos, float(50.0),  // 50m 范围内可见
                              uint32(301), bodyData)   // MSG_COMBAT_FLOAT_NTF
}
```

## 5. 死亡与复活

### 5.1 死亡处理

```das
def on_entity_killed(killerFullID : uint64; victimID : uint32)
{
    // 1. 添加 DeadTag
    // 注意：DeadTag 是 EnTT tag——通过 Bridge 操作
    let victimFull = make_full_entity_id(uint32(1), victimID)
    massive_set_tag(victimFull, "Dead", true)

    // 2. 清除仇恨表
    clear_threat_table(victimID)

    // 3. 清除 Buff（TODO: RemoveAllDebuffs bridge）

    // 4. 掉落 / 经验 / 任务进度（Phase 4+）

    massive_log_info("entity killed: victim={victimID} killer={killerFullID}")
}
```

### 5.2 复活

```das
def respawn_entity(entityID : uint32; posX, posY, posZ : float)
{
    let fullEid = make_full_entity_id(uint32(1), entityID)

    // 移除 DeadTag
    massive_set_tag(fullEid, "Dead", false)

    // 回复 HP
    massive_set_entity_health(fullEid, int32(100))  // TODO: Bridge 新增

    // 传送到复活点
    massive_set_entity_position(fullEid, float3(posX, posY, posZ))  // TODO: Bridge 新增

    massive_log_info("entity respawned: {entityID}")
}
```

## 6. C++ 侧集成——Phase 4 新增 Bridge 函数

```cpp
// MassiveModule 中追加

// 战斗
addExtern<DAS_BIND_FUN(massive_execute_damage)>(...);
//   int32 massive_execute_damage(uint64 attackerFullID, uint64 defenderFullID,
//                                int32 baseDamage, int32 damageType)
//   → 调用 DamagePipeline::Execute + 应用到 EnTT Health + 返回 finalDamage

// 标签操作（EnTT 操作——脚本不能直接写 EnTT）
addExtern<DAS_BIND_FUN(massive_set_tag)>(...);
//   void massive_set_tag(uint64 fullEntityId, string tag, bool value)
//   → EnTT 侧 emplace/remove Tag 组件

addExtern<DAS_BIND_FUN(massive_set_entity_health)>(...);
//   void massive_set_entity_health(uint64 fullEntityId, int32 value)
//   → 直接设为 EnTT Health.current = value

addExtern<DAS_BIND_FUN(massive_set_entity_position)>(...);
//   void massive_set_entity_position(uint64 fullEntityId, float3 pos)
//   → 直接设为 EnTT Position

// 战斗浮字
addExtern<DAS_BIND_FUN(massive_build_combat_float)>(...);
//   array<uint8> massive_build_combat_float(uint64 targetEid, int32 floatType,
//                                           int32 value, uint32 skillID)
```

## 7. 文件清单

```
Src/World/System/
├── DamagePipeline.h / .cpp       # 新建——伤害管线（C++ 静态方法）
├── MovementSystem.cpp            # 已有——Phase 2
├── RecalcStatsSystem.cpp         # 已有——Phase 3
└── CombatTimeoutSystem.cpp       # 已有——Phase 3

Scripts/
├── Combat.das                    # 新建——仇恨表 + 死亡/复活
├── Components.das                # 追加 ThreatEntry/ThreatTable
└── ServerTick.das                # 追加 combat_stage

Src/Proto/
├── Combat.proto                  # 新建——CombatFloatNtf
```

## 8. Phase 4 扩展点

| 功能 | Phase 4 状态 | 扩展内容 |
|------|-------------|---------|
| PRD 伪随机分布 | 简单 std::mt19937 | per-entity PRD 状态（BlobStorage） |
| 护盾吸收 | 未实现 | DamagePipeline 阶段 5 |
| Buff 伤害修正 | 未实现 | DamagePipeline 阶段 4——遍历 BuffList |
| AOE 伤害 | 未实现 | massive_entities_in_radius + 遍历目标 |
| DOT 伤害 | 周期 DamagePipeline 调用 | Buff 系统集成——on_buff_tick 中调用 |
