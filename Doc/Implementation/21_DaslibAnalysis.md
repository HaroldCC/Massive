# 脚本引擎 #21：DasLib 标准库使用分析

> 状态：**分析中**（2026-07-15）
> 关联：全部脚本引擎设计（#09-#20）
> 前置依赖：daScript 运行时已集成
> 影响 Phase：Phase 2（Bridge 简化）+ Phase 3（消息迁移简化）+ Phase 4（业务系统简化）

## 1. 分析结果总表

对 daslib 中 15 个模块的评估结果：

| 模块 | 评价 | 行动 |
|------|------|------|
| `math`（内建） | ✅ 内置 `distance`/`normalize`/`lerp`/`cross`/`dot`/`length` for `float3` | **删除**我们手写的 `distance_3d` / `normalize_3d` |
| `builtin.das` | ✅ 内置 `sort(arr)` + `sort(arr, cmp)` / `table<K,V>` 全套操作 | **删除**手写的排序和表操作工具函数 |
| `functional.das` | ✅ `filter`/`map`/`reduce`/`any`/`all`/`find` | **使用**它简化 AI 决策链和 Buff 遍历 |
| `linq.das` | ⚠️ 语法糖 vs functional.das | **SKIP** — functional.das 更轻量 |
| `json.das` | ⚠️ 可作为配置表格式 | **INVESTIGATE** Phase 4+ — 替代 ConfigTable 脚本绑定 |
| `random.das` | ✅ 内置 RNG | **使用**它替代 C++ 侧 `std::mt19937` 的脚本绑定 |
| `regex.das` | ✅ 内置正则 | **使用**它做聊天敏感词过滤 |
| `enum_trait.das` | ✅ `enum_names` / `string_to_enum` | **使用**它替代 buff type/state 的 int 常量 |
| `archive.das` | ✅ DECS 已依赖 | **无需额外操作** — decs_live 已正确使用 |
| `sort_boost.das` | ⚠️ `builtin.das` 已内置排序 | **SKIP** — builtin 的 sort 已足够 |
| `cpp_bind.das` | ❌ 代码生成工具（非脚本库） | **PHASE 5** — 评估是否能用它自动化 MassiveModule 生成 |
| `heartbeat.das` | ❌ 网络心跳，与游戏 Tick 无关 | **SKIP** |
| `json_boost.das` | ⚠️ JSON 增强 | **SKIP** — 和 json.das 重复；等 json.das 方案确认后再看 |
| `das_source_formatter.das` | ✅ 代码格式化 | **使用**它做 .das 代码规范检查 |
| `apply.das` | ✅ struct 字段遍历 | **SKIP** — 目前不需要动态反射 struct 字段 |

## 2. 核心发现

### 发现 1：`float3` 已内建全部向量运算

C++ 侧 `module_builtin_math.cpp` 第 688-713 行已注册：

```cpp
// daScript 的内建数学模块自动提供：
addExternEx<float3(float3,float3), DAS_BIND_FUN(distance3)>  -> "distance"   // distance(a,b)
addExternEx<float3(float3,float3), DAS_BIND_FUN(distanceSq3)> -> "distance_sq"
addExternEx<float3(float3),       DAS_BIND_FUN(safe_normalize3)> -> "normalize"
addExternEx<float3(float3,float3), DAS_BIND_FUN(cross3)>      -> "cross"
addExternEx<float3(float3,float3,float), DAS_BIND_FUN(lerp_vec_float)> -> "lerp"
```

**影响**：我们在 `Scripts/Common.das` / `AI.das` / `Skill.das` 中手写的 `distance_3d()` / `normalize_3d()` / `make_full_entity_id()` 不需要了。daScript 内建直接可用：

```das
// ✅ 内建 — 不需要手写
let dist = distance(pos, targetPos)
let dir = normalize(targetPos - pos)
let mid = lerp(a, b, 0.5f)
let len = length(v)
```

### 发现 2：内建 `sort()` 已支持自定义比较器

`builtin.das` 第 1542-1595 行：

```das
// 已内建 — 不需要 sort_boost.das
sort(arr)                          // 数字/字符串数组 — 默认升序
sort(arr, $(a, b) => b.threat < a.threat)  // 自定义比较器
```

**影响**：威胁排序、BuffList modifier 排序直接用内建 `sort()`，不需要我们手写排序函数。

### 发现 3：`functional.das` 提供标准函数式原语

```das
require daslib/functional

// filter — 替代手写 for-if
let aliveTargets = filter(targets) $(t) => !massive_entity_is_dead(t)

// any / all — 替代手写循环检查
let hasThreat = any(threats) $(t) => t.threat > 0

// find — 替代手写 for-break
let cd = find(cds.entries) $(c) => c.skillID == skillID

// sum — 替代手写累加
let totalThreat = sum(threats) $(t) => t.threat
```

### 发现 4：`table<K,V>` 已内建全部操作

不需要我们手写任何哈希表操作：

```das
var handlers : table<uint32; block<(argsPtr : void?) : void>>
handlers[MSG_MOVE_REQ] <- @@(argsPtr) { ... }
let h = find(handlers, MSG_MOVE_REQ)
let allKeys = keys(handlers)
```

## 3. 需修改的文档

| 文档 | 修改项 | 优先级 |
|------|--------|--------|
| `17_AISystem.md` | `distance_3d` / `normalize_3d` → 内建 `distance()` / `normalize()` | ✅ 已修改 |
| `15_SkillSystem.md` | `distance_3d` → 内建 `distance()` | Phase 4 |
| `16_CombatSystem.md` | 威胁排序 — 用内建 `sort(arr, cmp)`；`massive_random()` → `random.das` 内建 RNG | Phase 4 |
| `14_BuffSystem.md` | BuffList modifier 排序 — 用内建 `sort(arr, cmp)` | Phase 4 |
| `10_BridgeModule.md` | 删除 `massive_random()` 桥接 — daScript `random.das` 已内建；删除未使用的标签桥接 | Phase 2 |

## 4. Agent 补充发现（之前手动分析遗漏）

| 模块 | 发现 | 影响 |
|------|------|------|
| `delegate.das` | C# 风格多播委托——`delegate<lambda<(args):ret>>` + `+=` / `invoke` | **可用于替代部分 `ecs_dispatch`**——同一 context 内的回调注册。DECS 已有 `[decs_event]` 注解，但 delegate 更轻量 |
| `random.das` | 内置 LCG RNG——`random_int`/`random_float`/`random_unit_vector` | **删除 `massive_random_float()` Bridge 函数**——脚本侧直接用 `random_float()` |
| `debug.das` | DAP debugger 的 `DapiDebugAgent.onInstrumentFunction` 钩子 | **反向集成 Tracy**——可实现在 Tracy 中看到 daScript 函数级火焰图 |
| `archive.das` | DECS 内部已用——可扩展到非 DECS 持久化 | Phase 4+ — 替代 Protobuf 做本地存档

## 4. 不修改的内容

以下我们的设计不需要改——它们恰当地使用了 Bridge 做 C++ ↔ daScript 边界：

| 保留项 | 理由 |
|--------|------|
| 18 个 Bridge 函数 | daScript 内建 math 无法跨越 C++ EnTT 边界——`massive_entity_position()` 必须存在 |
| `massive_schedule_timer` | daScript 无内建定时器——必须桥接 C++ TimingWheel |
| `massive_send_to_client` | daScript 无网络层——必须桥接 C++ Gate/World 网络栈 |
| `massive_execute_damage` | 伤害管线走 C++ 热路径——不能搬进 daScript |
| Protobuf C++ 预解析 | daScript 无 protobuf 库——不能替代 |

## 5. 引用

| 项 | 源码位置 |
|----|---------|
| `float3.distance/normalize/cross/lerp` | `ThirdParty/daScript/src/builtin/module_builtin_math.cpp:688-720` |
| `sort(arr)` / `sort(arr, cmp)` | `ThirdParty/daScript/daslib/builtin.das:1542-1622` |
| `table<K,V>` API | `ThirdParty/daScript/daslib/builtin.das` |
| `functional.das` — filter/map/reduce/any/all/find | `ThirdParty/daScript/daslib/functional.das:22-240` |
| `random.das` | `ThirdParty/daScript/daslib/random.das` |
| `regex.das` | `ThirdParty/daScript/daslib/regex.das` |
| `enum_trait.das` | `ThirdParty/daScript/daslib/enum_trait.das` |
