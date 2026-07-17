# 脚本引擎 #15：技能系统——施法管线 + 冷却管理

> 状态：**设计中**（2026-07-15）
> 关联：[09_ScriptEngine](09_ScriptEngine.md)（总纲 §4.2 DECS components）、[13_MessageMigration](13_MessageMigration.md)（消息分派）、[skill_system](../前期设计/ecs/skill_system.html)（上游设计）
> 前置依赖：Phase 2 完成（MassiveModule/EntityManager）、Phase 3 完成（消息迁移 framework）、定时器可用
> 对应 Phase：Phase 4

## 1. 定位

技能系统在 DECS 中实现，覆盖技能的完整生命周期。采用 `skill_system.html` 的 6 阶段管线设计，但在 DECS 架构下重新实现——所有状态（冷却记录/施法状态）存在 DECS Component 中。

## 2. DECS 数据结构

```das
// Scripts/Components.das — 技能相关定义

// ── 冷却记录 ──
[decs_template]
struct CooldownEntry {
    skillID : uint32
    endTime : float     // 冷却结束时刻（服务器时间秒）
}

[decs_template]
struct Cooldowns {
    entries    : array<CooldownEntry>
    gcdEndTime : float   // GCD 结束时刻，0 = 不在 GCD
}

// ── 施法状态 ──
[decs_template]
struct CastState {
    skillID   : uint32
    state     : int     // 0=IDLE, 1=CASTING, 2=CHANNELING
    target    : uint64  // 目标 fullEntityId
    targetPos : float3  // AoE 坐标
    timerID   : uint32  // 读条定时器 ID
}

// ── 技能栏 ──
[decs_template]
struct SkillSlot {
    skillID : uint32
    level   : int
}

[decs_template]
struct SkillBar {
    slots : array<SkillSlot>
}
```

## 3. 施法管线（DECS 实现）

### 3.1 入口：`handle_skill_cast`

```das
// Scripts/Skill.das
require daslib/decs_boost
require massive

let SKILL_OK              = 0
let SKILL_ERR_NOT_FOUND   = 1
let SKILL_ERR_ON_COOLDOWN = 2
let SKILL_ERR_GCD_ACTIVE  = 3
let SKILL_ERR_NOT_ENOUGH_MP = 4
let SKILL_ERR_CANT_CAST   = 9
let SKILL_ERR_OUT_OF_RANGE = 7
let SKILL_ERR_INVALID_TARGET = 6

// 由 C++ switch case 投递——Protobuf 已反序列化
[export]
def handle_skill_cast(sessionID : uint32; skillID : uint32;
                      targetEid : uint64; targetX, targetY, targetZ : float)
{
    // 1. 查 caster
    let casterFull = massive_find_entity_by_session(sessionID)
    if casterFull == uint64(0) { return }
    let casterID = extract_entity_id(casterFull)

    // ── 阶段 1: 基础校验 ──
    let err = check_cast_preconditions(casterID, skillID)
    if err != SKILL_OK {
        send_skill_cast_rsp(sessionID, skillID, err)
        return
    }

    // ── 阶段 2: 资源校验 ──
    // Phase 4: 简化——暂不做 MP 扣减（BattleStats 在 EnTT 中，需 Bridge 函数）
    // TODO: let stats = massive_entity_get_battlestats(casterFull)
    // if stats != null { if stats.currentMp < template.cost_mp { ... } }

    // ── 阶段 3: 目标校验 ──
    let targetID = extract_entity_id(targetEid)
    if targetEid != uint64(0) {
        let casterPos = massive_entity_position(casterFull)
        let targetPos = massive_entity_position(targetEid)
        let dist = distance_3d(casterPos, targetPos)
        let maxRange = float(20)  // TODO: 模板读取
        if dist > maxRange {
            send_skill_cast_rsp(sessionID, skillID, SKILL_ERR_OUT_OF_RANGE)
            return
        }
    }

    // ── 阶段 4: 扣资源 + 设冷却（先扣再执行）──
    query(casterID) $(var cds : Cooldowns) {
        let cooldownMs = 3000  // TODO: 模板读取
        if cooldownMs > 0 {
            set_cooldown_inline(cds, skillID, massive_get_tick_time() + float(cooldownMs) / 1000.0f)
        }
        let gcdMs = 1500  // TODO: 模板读取
        if gcdMs > 0 {
            cds.gcdEndTime = massive_get_tick_time() + float(gcdMs) / 1000.0f
        }
    }

    // ── 阶段 5: 读条—瞬发分支 ──
    let castTimeMs = 0  // TODO: 模板读取
    if castTimeMs > 0 {
        // 读条：注册定时器
        let timerID = massive_schedule_timer(castTimeMs) <| @(tid : uint32) {
            execute_skill_effect(casterID, skillID, targetEid)
            set_cast_state(casterID, 0)  // IDLE
        }
        set_cast_state(casterID, 1)  // CASTING
    } else {
        // 瞬发：立即执行
        execute_skill_effect(casterID, skillID, targetEid)
    }

    // 回复客户端
    send_skill_cast_rsp(sessionID, skillID, SKILL_OK)
}
```

### 3.2 阶段 1 详细校验

```das
def check_cast_preconditions(entityID : uint32; skillID : uint32) : int
{
    // 检查 entity 是否死亡/晕眩（Bridge 查询 EnTT tags）
    let fullEid = make_full_entity_id(uint32(1), entityID)  // TODO: sceneID 获取
    if massive_entity_is_dead(fullEid)    { return SKILL_ERR_CANT_CAST }
    if massive_entity_is_stunned(fullEid) { return SKILL_ERR_CANT_CAST }

    // 检查 CastState
    query(entityID) $(var cs : CastState; var cds : Cooldowns) {
        // 读条中
        if cs.state != 0 {
            return SKILL_ERR_CANT_CAST
        }

        // GCD 检查
        if cds.gcdEndTime > massive_get_tick_time() {
            return SKILL_ERR_GCD_ACTIVE
        }

        // 冷却检查
        for cd in cds.entries {
            if cd.skillID == skillID && cd.endTime > massive_get_tick_time() {
                return SKILL_ERR_ON_COOLDOWN
            }
        }
    }

    return SKILL_OK
}
```

### 3.3 效果执行

```das
def execute_skill_effect(casterID : uint32; skillID : uint32; targetEid : uint64)
{
    let targetID = extract_entity_id(targetEid)
    if targetID == uint32(0) { return }

    // Phase 4 简化效果：对目标施加 HealthModifier（-20 HP 伤害）
    query(targetID) $(var hm : HealthModifier) {
        hm.flatDelta -= 20  // 扣 20 HP
    }

    // Phase 4+ 扩展：调用 DamagePipeline
    // let result = execute_damage_pipeline(casterID, targetID, skillID, 20)

    massive_log_info("skill effect: caster={casterID} skill={skillID} target={targetID}")
}
```

## 4. 冷却管理

```das
// 设置冷却（内联到 query 闭包中）
def set_cooldown_inline(var cds : Cooldowns; skillID : uint32; endTime : float)
{
    for var cd in cds.entries {
        if cd.skillID == skillID {
            cd.endTime = endTime
            return
        }
    }
    let newEntry = CooldownEntry{skillID = skillID, endTime = endTime}
    cds.entries |> push(newEntry)
}

// 获取剩余冷却时间
def get_cooldown_remaining(cds : Cooldowns; skillID : uint32) : float
{
    for cd in cds.entries {
        if cd.skillID == skillID {
            return max(0.0f, cd.endTime - massive_get_tick_time())
        }
    }
    return 0.0f
}
```

### 4.1 冷却 Tick——`ecs_stage("skill_cd")`

```das
// 每 Tick 清理过期冷却记录
[decs(stage = "skill_cd")]
def system_skill_cooldown()
{
    let now = massive_get_tick_time()

    query() $(var cds : Cooldowns) {
        for var i = length(cds.entries) - 1; i >= 0; i-- {
            if cds.entries[i].endTime <= now {
                cds.entries |> erase(i)
            }
        }

        // GCD 过期
        if cds.gcdEndTime > 0.0f && cds.gcdEndTime <= now {
            cds.gcdEndTime = 0.0f
        }
    }
}
```

## 5. 施法打断

```das
def interrupt_casting(entityID : uint32)
{
    query(entityID) $(var cs : CastState) {
        if cs.state == 0 { return }  // 没有在施法

        // 取消读条定时器
        if cs.timerID != uint32(0) {
            massive_cancel_timer(cs.timerID)
        }

        cs.state = 0  // IDLE
        cs.timerID = uint32(0)
    }
}

// 施法状态写
def set_cast_state(entityID : uint32; state : int)
{
    query(entityID) $(var cs : CastState) {
        cs.state = state
    }
}
```

## 6. 消息协议（Phase 4 新增 Proto）

```protobuf
// Src/Proto/Skill.proto（新建）
syntax = "proto3";
package MMO.Proto;

message SkillCastReq {
    uint32 skill_id = 1;
    uint64 target_entity_id = 2;   // uint64 fullEntityId
    float  target_x = 3;
    float  target_y = 4;
    float  target_z = 5;
}

message SkillCastRsp {
    uint32 skill_id = 1;
    uint32 error_code = 2;         // 0=success
    float  cooldown_end = 3;
    float  gcd_end = 4;
}

message SkillCastNtf {
    uint64 caster_entity_id = 1;   // 施法者
    uint32 skill_id = 2;
    uint64 target_entity_id = 3;
    float  target_x = 4;
    float  target_y = 5;
    float  target_z = 6;
    int32  cast_time_ms = 7;       // 读条时间（客户端播施法动画）
}

enum SkillErrorCode {
    SKILL_OK               = 0;
    SKILL_ERR_NOT_FOUND    = 1;
    SKILL_ERR_ON_COOLDOWN  = 2;
    SKILL_ERR_GCD_ACTIVE   = 3;
    SKILL_ERR_NOT_ENOUGH_MP = 4;
    SKILL_ERR_NOT_ENOUGH_HP = 5;
    SKILL_ERR_INVALID_TARGET = 6;
    SKILL_ERR_TARGET_DEAD  = 7;
    SKILL_ERR_OUT_OF_RANGE = 8;
    SKILL_ERR_CANT_CAST    = 9;
    SKILL_ERR_ALREADY_CASTING = 10;
    SKILL_ERR_INTERRUPTED  = 11;
}
```

## 7. Stage 调度

```das
// ServerTick.das 的 update() 中追加
def update(sceneID : uint32; dt : float)
{
    decs_stage("input")       // 消息处理
    decs_stage("buff_tick")   // Buff 到期/周期触发
    decs_stage("skill_cd")    // 冷却递减 + GCD (+ 技能 CD 递减)
    decs_stage("ai_decision") // AI 行为树（Phase 4 后续）
    decs_stage("dirty_flush") // 脏标记
}
```

## 8. 文件清单

```
Scripts/
├── Skill.das                      # 新建——技能系统（施法管线+冷却管理）
├── Components.das                 # 追加 Cooldowns/CastState/SkillBar
└── ServerTick.das                 # 追加 ecs_stage("skill_cd")

Src/Proto/
├── Skill.proto                    # 新建——技能消息协议
├── MsgID.proto                    # 追加 MSG_SKILL_CAST_REQ/RSP/NTF
```

## 9. Phase 4 扩展点

| 功能 | Phase 4 状态 | 扩展内容 |
|------|-------------|---------|
| 技能模板（ConfigTable） | 硬编码 skillID → 参数 | 脚本侧 ConfigTable 绑定 |
| MP/HP 资源扣减 | 未实现 | `massive_entity_get_battlestats()` Bridge 读整体属性 + `massive_set_entity_health()` Bridge 写 |
| 伤害管线（DamagePipeline） | 硬编码 -20 HP | C++ 侧 DamagePipeline + 脚本绑定 |
| 技能脚本回调（on_cast_script） | 未实现 | 动态脚本调用——按模板的脚本函数名 invoke |
| AoE 技能 | 未实现 | `massive_entities_in_radius()` 收集目标列表 |
| 读条动画广播 | 未实现 | `massive_broadcast_nearby()` 发送 SkillCastNtf |
| 引导技能（Channeling） | 未实现 | 周期性 Tick + 定时器链 |
| GCD 急速计算 | 硬编码 1.5s | attackSpeed 万分比修正 |
