# 脚本引擎 #17：AI 系统——行为树 + 黑板 + LOD

> 状态：**设计中**（2026-07-15）
> 关联：[09_ScriptEngine](09_ScriptEngine.md)（总纲 §4.2 DECS components）、[16_CombatSystem](16_CombatSystem.md)（仇恨系统）、[ai_system](../前期设计/ecs/ai_system.html)（上游设计）
> 前置依赖：Phase 3 消息迁移完成、CombatSystem 可用（仇恨表）、定时器可用
> 对应 Phase：Phase 4

## 1. 定位

AI 系统驱动所有非玩家实体（怪物、NPC、宠物）的自主行为。采用 `ai_system.html` 定义的**行为树 + 黑板**架构，适配到 DECS 环境：

| 组件 | 位置 | 说明 |
|------|------|------|
| **AIBlackboard** | DECS Component | AI 状态存储——当前目标、巡逻索引、技能列表等 |
| **行为树引擎** | DECS 侧 DasLang 函数 | 每个 AI 实体的 `RunBehaviorTree()` 函数 |
| **AI 调度器** | C++ 侧 → `ecs_stage("ai_decision")` | 固定频率 Tick，控制分帧和处理预算 |
| **LOD 系统** | 脚本侧 | 根据与玩家距离降级 AI 频率 |

## 2. DECS 数据结构

```das
// Scripts/Components.das — AI 相关

[decs_template]
struct AIBlackboard {
    // 目标管理
    currentTarget    : uint64     // 当前目标 fullEntityId
    threatTarget     : uint64     // 仇恨最高目标
    aggroTarget      : uint64     // 进入战斗时的初始攻击者

    // 状态机
    aiState          : int        // 0=IDLE, 1=PATROL, 2=COMBAT, 3=FLEE, 4=RETURN, 5=DEAD
    lastStateChange  : float      // 状态切换时间

    // 巡逻
    patrolPathID     : int        // 巡逻路径 ID（0=无巡逻）
    patrolIndex      : int        // 当前巡逻点索引
    patrolDirection  : int        // 1=正序, -1=逆序
    patrolWaitUntil  : float      // 巡逻点等待到何时

    // 出生点
    spawnPosition    : float3     // 出生/返回位置
    chaseRange       : float      // 追击范围
    leashRange       : float      // 脱战范围（超出则回出生点）

    // 技能
    availableSkillIDs : array<uint32>  // 可用技能列表
    lastSkillCastTime : float          // 上次施法时间

    // LOD
    lodLevel         : int        // 0=Full, 1=Medium, 2=Minimal
    lastAiTickTime   : float      // 上次 AI Tick 时间
}

// AI 基础属性（EnTT 组件——每 Tick 快速查找）
// MonsterAIConfig: chaseRange / leashRange / patrolPathID / skillIDs
// → 从模板初始化后存在 EnTT 中，脚本通过 Bridge 读
```

### 2.1 EnTT 侧 AI 配置

```cpp
// Src/Common/ECS/MonsterConfig.h（新建）
struct MonsterAIConfig {
    float  chaseRange     = 50.0f;  // 追击范围（米）
    float  leashRange     = 100.0f; // 脱战范围
    float  aiTickInterval = 0.2f;   // AI 决策间隔（秒）
    int32_t patrolPathID = 0;       // 0=无巡逻
    std::vector<uint32_t> skillIDs; // 可用技能列表
};
```

## 3. 行为树节点

```das
// Scripts/AI.das
require daslib/decs_boost
require massive

let BT_SUCCESS = 0
let BT_FAIL    = 1
let BT_RUNNING = 2

// ── 条件节点 ──

// 检查是否有仇恨目标
def bt_cond_has_threat_target(entityID : uint32) : int
{
    var hasTarget = false
    query(entityID) $(var bb : AIBlackboard) {
        bb.threatTarget = get_top_threat_target(entityID)
        if bb.threatTarget != uint64(0) {
            bb.currentTarget = bb.threatTarget
            bb.aiState = 2  // COMBAT
            hasTarget = true
        }
    }
    return hasTarget ? BT_SUCCESS : BT_FAIL
}

// 检查目标是否在近战范围
def bt_cond_in_melee_range(entityID : uint32) : int
{
    let fullEid = make_full_entity_id(uint32(1), entityID)
    let pos = massive_entity_position(fullEid)

    var targetEid = uint64(0)
    query(entityID) $(var bb : AIBlackboard) {
        targetEid = bb.currentTarget
    }

    if targetEid == uint64(0) { return BT_FAIL }

    let targetPos = massive_entity_position(targetEid)
    let dist = distance(pos, targetPos)  // ← daScript 内建
    return dist <= 2.0f ? BT_SUCCESS : BT_FAIL
}

// 检查目标是否在追击范围内
def bt_cond_in_chase_range(entityID : uint32; chaseRange : float) : int
{
    let fullEid = make_full_entity_id(uint32(1), entityID)
    let pos = massive_entity_position(fullEid)

    var targetEid = uint64(0)
    query(entityID) $(var bb : AIBlackboard) {
        targetEid = bb.currentTarget
    }
    if targetEid == uint64(0) { return BT_FAIL }

    let targetPos = massive_entity_position(targetEid)
    let dist = distance(pos, targetPos)  // ← daScript 内建
    return dist <= chaseRange ? BT_SUCCESS : BT_FAIL
}

// ── 动作节点 ──

// 向目标移动
def bt_action_move_to_target(entityID : uint32; moveSpeed : float)
{
    let fullEid = make_full_entity_id(uint32(1), entityID)
    let pos = massive_entity_position(fullEid)

    var targetEid = uint64(0)
    query(entityID) $(var bb : AIBlackboard) {
        targetEid = bb.currentTarget
    }
    if targetEid == uint64(0) { return; }

    let targetPos = massive_entity_position(targetEid)
    let dir = normalize(targetPos - pos)
    let newPos = pos + dir * moveSpeed * massive_get_dt()

    massive_set_entity_position(fullEid, newPos)
}

// 攻击目标
def bt_action_attack_target(entityID : uint32; skillIDs : array<uint32>)
{
    var targetEid = uint64(0)
    query(entityID) $(var bb : AIBlackboard) {
        targetEid = bb.currentTarget
        bb.lastSkillCastTime = massive_get_tick_time()
    }

    if targetEid == uint64(0) { return; }

    // 选择第一个不在 CD 中的技能
    if length(skillIDs) > 0 {
        let skillID = skillIDs[0]  // Phase 4 简化：只用一个技能
        handle_skill_cast_ai(entityID, skillID, targetEid)
    }
}

// 返回出生点
def bt_action_return_to_spawn(entityID : uint32; spawnPos : float3; moveSpeed : float)
{
    let fullEid = make_full_entity_id(uint32(1), entityID)
    let pos = massive_entity_position(fullEid)
    let dist = distance_3d(pos, spawnPos)

    if dist < 1.0f {
        // 到达——重置 AI 状态
        query(entityID) $(var bb : AIBlackboard) {
            bb.aiState = 0  // IDLE
            bb.currentTarget = uint64(0)
        }
        clear_threat_table(entityID)
        return
    }

    // 向出生点移动
    let dir = normalize(spawnPos - pos)
    let newPos = pos + dir * moveSpeed * massive_get_dt()
    massive_set_entity_position(fullEid, newPos)
}
```

## 4. 行为树运行——怪物通用模板

```das
// 怪物通用 AI——在 ecs_stage("ai_decision") 中对每个怪物调用
def run_monster_ai(entityID : uint32; chaseRange : float; leashRange : float;
                   moveSpeed : float; skillIDs : array<uint32>)
{
    let fullEid = make_full_entity_id(uint32(1), entityID)

    // 检查死亡——死亡的实体跳过 AI
    if massive_entity_is_dead(fullEid) { return }

    var spawnPos = float3()
    query(entityID) $(var bb : AIBlackboard) {
        spawnPos = bb.spawnPosition
    }

    // ── 分支 1: 战斗 ──
    if bt_cond_has_threat_target(entityID) == BT_SUCCESS {
        if bt_cond_in_chase_range(entityID, chaseRange) == BT_SUCCESS {
            // 在追击范围内
            if bt_cond_in_melee_range(entityID) == BT_SUCCESS {
                // 近战范围——攻击
                bt_action_attack_target(entityID, skillIDs)
            } else {
                // 不在近战范围——移动
                bt_action_move_to_target(entityID, moveSpeed)
            }
            return
        } else {
            // 超出追击范围——脱战
            clear_threat_table(entityID)
            query(entityID) $(var bb : AIBlackboard) {
                bb.aiState = 4  // RETURN
                bb.currentTarget = uint64(0)
            }
            bt_action_return_to_spawn(entityID, spawnPos, moveSpeed)
            return
        }
    }

    // ── 分支 2: 返回出生点 ──
    var state = 0
    query(entityID) $(var bb : AIBlackboard) {
        state = bb.aiState
    }
    if state == 4 {  // RETURN
        bt_action_return_to_spawn(entityID, spawnPos, moveSpeed)
        return
    }

    // ── 分支 3: 巡逻 ──
    // Phase 4 简化：没有巡逻路径的怪物站桩

    // ── 分支 4: 待机（什么都不做）──
}
```

## 5. AI 调度——`ecs_stage("ai_decision")`

```das
// AI 调度器——每 Tick 限处理 N 个 monster，防止预算溢出
var g_aiCursor = uint32(0)  // Round-Robin cursor

[decs(stage = "ai_decision")]
def system_ai_decision()
{
    // Phase 4 简化：遍历所有 monster，每个 monster 检查是否需要 Tick
    let now = massive_get_tick_time()
    
    query() $(var bb : AIBlackboard) {
        let entityID = get_eid()

        // LOD 时间间隔检查
        let interval = get_ai_tick_interval(bb.lodLevel)
        if now - bb.lastAiTickTime < interval { continue }

        bb.lastAiTickTime = now

        // 更新 LOD 级别
        bb.lodLevel = calc_ai_lod(entityID)

        // 运行行为树
        run_monster_ai(entityID, float(50.0), float(100.0),  // chaseRange, leashRange
                       float(5.0),                             // moveSpeed
                       empty_array_uint32())                   // skillIDs (TODO)
    }
}
```

### 5.1 AI LOD 计算

```das
let LOD_FULL    = 0  // 0-50m:  每 200ms 完整 AI
let LOD_MEDIUM  = 1  // 50-150m: 每 1s 简化 AI
let LOD_MINIMAL = 2  // 150m+:   每 5s 最小 AI

def calc_ai_lod(entityID : uint32) : int
{
    // 获取到最近玩家的距离（TODO: AOI 查询或范围估算）
    // Phase 4 简化：范围估算——不精确但够用
    let fullEid = make_full_entity_id(uint32(1), entityID)
    let pos = massive_entity_position(fullEid)

    // 查 50m 范围内有多少 entity（用范围查询估算密度）
    let nearby = massive_entities_in_radius(pos, float(50.0))

    // Phase 4 简化：附近有 entity → Full，没有 → Medium
    // 生产期改为查询最近玩家距离
    if length(nearby) > 0 { return LOD_FULL }
    return LOD_MEDIUM
}

def get_ai_tick_interval(lod : int) : float
{
    if lod == LOD_FULL    { return 0.2f }   // 200ms
    if lod == LOD_MEDIUM  { return 1.0f }   // 1s
    return 5.0f                              // 5s
}
```

## 6. AI 初始化——创建 monster 时

```das
// 创建 monster entity 时初始化 AIBlackboard
def init_monster_ai(entityID : uint32; spawnX, spawnY, spawnZ : float;
                    chaseRange : float; leashRange : float)
{
    create_entity() @(eid2, cmp) {
        cmp.eid := entityID
    }

    query(entityID) $(var bb : AIBlackboard) {
        bb.aiState = 0  // IDLE
        bb.spawnPosition = float3(spawnX, spawnY, spawnZ)
        bb.chaseRange = chaseRange
        bb.leashRange = leashRange
        bb.lodLevel = LOD_FULL
        bb.lastAiTickTime = massive_get_tick_time()
    }

    commit()
}
```

## 7. 文件清单

```
Scripts/
├── AI.das                         # 新建——行为树节点 + 怪物 AI 模板 + LOD
├── Combat.das                     # 新增——辅助函数（distance_3d, normalize_3d）
├── Components.das                 # 追加 AIBlackboard
└── ServerTick.das                 # 追加 ecs_stage("ai_decision")

Src/Common/ECS/
├── MonsterConfig.h                # 新建——MonsterAIConfig struct
└── MassiveModule.h               # 追加 Bridge: massive_set_entity_position
```

## 8. 依赖的 daslib 模块

| 功能 | 来源 | 替代手写 |
|------|------|---------|
| `distance(a, b)` — 3D 距离 | daScript 内建 `math` 模块 | ❌ 不需要 `distance_3d()` |
| `normalize(v)` — 向量归一化 | daScript 内建 `math` 模块 | ❌ 不需要 `normalize_3d()` |
| `length(v)` — 向量长度 | daScript 内建 `math` 模块 | — |
| `make_full_entity_id(sceneID, eid)` | 保留手写 — 仅 1 行 | — |

> **引用**: `ThirdParty/daScript/src/builtin/module_builtin_math.cpp:688-713`

## 9. Phase 4 扩展点

| 功能 | Phase 4 状态 | 扩展内容 |
|------|-------------|---------|
| 完整行为树引擎 | 硬编码 if-else | 独立的 C++ 行为树引擎 + DasLang 节点注册 |
| NPC 日程系统 | 未实现 | NPCSchedule Component + 游戏内时间驱动 |
| 巡逻路径 | 未实现 | PatrolPath 数据 + pathIndex 切换逻辑 |
| 逃跑 AI | 未实现 | hpPercent 检查 + FleeAction |
| AI 技能选择 | 只用 skillIDs[0] | 复杂选择算法（优先级、冷却、位置） |
| 寻路 | 简单位移（直线移动） | NavMesh 寻路——Future |
| LOD 精确距离 | 范围估算 | 空间索引查询到最近玩家的距离 |
| 分帧调度 | 全量遍历（小规模） | C++ 侧 Round-Robin + aiBatchSize 预算控制 |
