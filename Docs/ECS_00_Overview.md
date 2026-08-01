# 实体/ECS 层重构总览（ECS_00_Overview）

> 本文是 Massive 游戏实体层（ECS + 脚本玩法）重构的执行文档总纲。系列共 9 篇，
> 本篇给出全局蓝图、**权威契约**（uint64 布局、目录、命名）、分阶段路线，
> 以及**必须由你拍板的分歧点**；后续 8 篇给出可照抄的实现（主体代码 + codegen + 构建 + 测试）。
>
> - `ECS_00_Overview.md`（本篇）— 蓝图、契约、目录、分层、分阶段路线、待拍板项
> - `ECS_01_Identity.md` — 身份系统：uint64 EntityID、EntityRegistry（index+version）、组合层 Prefab、玩家 identity 归位
> - `ECS_02_Components.md` — 组件集（C++ 固定）：组件声明、DirtyIndex、组件所有权模型
> - `ECS_03_StageScheduler.md` — Stage 调度器 + dt 管线：World::Tick(dt)、8 阶段、SceneManager 遍历 API、LogicThread 改造
> - `ECS_04_SpatialAOI.md` — 均匀格子空间索引 + 增量 AOI（enter/leave 事件流）+ 活跃集
> - `ECS_05_Replication.md` — 复制系统：dirty-driven + 带宽预算 + 并行打包 + REPLICATE_COMPONENT codegen
> - `ECS_06_ScriptBridge.md` — 脚本绑定面：Bridge_* extern、EntityBatch、组件访问器、热重载/AOT 契约
> - `ECS_07_ScriptMacros.md` — daslang 宏 `[game_system]` / `[game_event]`：注解、注册、调度
> - `ECS_08_Build_Tests.md` — 构建脚本（xmake rule）、codegen 脚本落地、测试用例

---

## 0. 本篇定位与阅读顺序

本轮把游戏实体层从「一个基于 EnTT 的空壳（模拟管线全是死代码）」重构为**可交付的实体层底座**：C++ 拥有全部组件数据并跑热路径，daslang 承担主要玩法逻辑，支撑**单场景 5w+ 实体、同屏上千**的规模。

已核实的现状（源码逐字确认，非推断）：

- **整条模拟管线是死代码**：`WorldServer::OnTick`（`Src/World/WorldServer.cpp:238`）只做 `ProcessUnroutedMessages` + `ProcessControlMessages` + `UpdateLoadLevel`；`RunCPPSystems`/`SystemMovement`/`SystemAOI`/`WorldServer::SystemReplicate`/`DirtyTracker<T>` **全部零调用点**（grep 确认）。
- **重构从「什么都没跑」开始** —— 这是自由，没有任何现有接线是载荷验证过、拆不得的。
- **`Scene::RegisterScriptComponent`**（`Src/Common/ECS/Scene.h:206`）声明存在、**无定义无调用**，是删脚本层 ECS（commit `1d3f1a4a`）留下的链接期地雷，本轮删除。

阅读顺序：先读本篇（契约 + 分歧），拍板分歧点后，按 `01 → 08` 顺序实施。`01/02/03` 是地基（任何东西都依赖它），`04/05` 是性能墙，`06/07` 是脚本接缝,`08` 收尾构建与测试。

---

## 1. 设计铁律（贯穿全系列，不可违背）

这两条是 5w+ 规模 + daslang 主导玩法 + 热重载三个约束交汇后的唯一可行解。任何细节篇的设计若与之冲突，以本节为准。

> **铁律 1 —— 组件数据永远只活在 C++/EnTT，绝不进 das context/heap。**
> 脚本只持有 `uint64 entityID`（纯整数）和**临时**组件值拷贝 / `EntityBatch` 句柄。
> 热重载 context swap 时：EnTT 组件数据原封不动（零丢失），脚本侧注册表/值拷贝随旧 context 销毁后由新 context 的 `[init]` 重建。
> 依据：`DasLangEngine::DoSwap`（`Src/ScriptEngine/DasEngine.cpp:284`）`_scriptImage = std::move(img)` 直接丢弃旧 Context。被删的 `ScriptComponentStorage`（string-key untyped byte-blob 并行存储）就是违反此律的反面教材。

> **铁律 2 —— 脚本调用量 ∝ 事件数，绝不 ∝ 实体数 × tick。**
> 5w × 50Hz 的每实体每 tick 脚本调用 = 250 万次/秒跨 AOT/解释器边界，必死。
> 因此：C++ 跑所有 O(实体数) 的热循环（移动积分、格子索引、增量 AOI、复制 diff）；daslang 只跑离散事件（伤害/死亡/拾取/进出 AOI/技能命中）和**分频错峰**的低频决策（AI/刷怪/Buff 到期）。玩法复杂度在事件逻辑里，不在每帧积分里 —— 所以「脚本主导玩法」与「大规模性能」不冲突。

**数据 = C++ 拥有（保性能与热重载安全）；行为 = daslang 拥有（保开发效率与热更）。** 这条线两边都能大放异彩。

---

## 2. 分层架构

```
┌──────────────────────────────────────────────────────────────┐
│  daslang 规则层（热重载 / AOT）—— Script/                       │
│    · [msg_handler]  网络消息 → 脚本（已存在，Script/Common/）    │
│    · [game_event]   游戏事件 → 脚本（本轮新增，ECS_07）          │
│    · [game_system]  低频决策 system → 脚本（本轮新增，ECS_07）   │
│    —— 调用量 ∝ 事件数（铁律 2）——                               │
└───────────────▲────────────────────────────┬──────────────────┘
    Bridge_* extern（读写组件/查询/生成）  │ 事件出队（C++ → 脚本）
                │                            ▼
┌───────────────┴────────────────────────────────────────────────┐
│  C++ 物理层（单线程模拟）—— Src/World/ + Src/Common/ECS/          │
│   EntityRegistry（uint64 ↔ entt::entity，index+version 自管）    │
│   Scene（entt::registry 持有者，组件唯一所有者）                  │
│   StageScheduler: PreUpdate→ScriptLogic→Movement→SpatialIndex    │
│                   →AOI→EventDispatch→PostUpdate→[并行]Replicate   │
│   Grid（均匀格子空间索引）· ActiveSet（活跃集）· DirtyIndex       │
└───────────────┬─────────────────────────────────────────────────┘
                │ 只读快照（模拟结束后）
                ▼   线程池并行，每玩家一个打包任务
        复制打包 → 带宽预算+优先级 → Aes256Gcm → Gate
```

**依赖方向铁律（沿用脚本层已定规则）**：`Src/ScriptEngine/` 是服务无关下层库，**不得反向依赖 World/Social**。实体层的归属：

| 组件 | 归属目录 | 理由 |
|---|---|---|
| `Entity` / `Scene` / `EntityRegistry` / `Grid` / `DirtyIndex` / `ActiveSet` | `Src/Common/ECS/`（target `CommonECS`） | 跨服务通用的 ECS 基建（World/Social 都可能用） |
| 具体组件（`Position`/`Health`/…） | `Src/World/Component/` | World 专用玩法数据 |
| `StageScheduler` / `Grid` 的 AOI 逻辑 / 复制 | `Src/World/`（target `WorldServer`） | World 专用（Social 无场景模拟） |
| `Bridge_*` 绑定模块 `WorldBridgeModule` | `Src/World/DasModule/` | World 的 `IDasLangModuleProvider` 实现 |
| `[game_system]`/`[game_event]` 宏 | `Script/Common/`（与 `[msg_handler]` 同处） | daslang 编译期宏，服务无关 |

---

## 3. 权威契约（所有细节篇必须逐字遵循）

### 3.1 uint64 EntityID 位布局

这是全系统最底层的契约：网络 id、脚本句柄、DB 回写、复制 diff 全用它。

```
 63          48 47                      20 19                  0
┌──────────────┬──────────────────────────┬────────────────────┐
│  scene (16)  │        index (28)         │    version (20)    │
└──────────────┴──────────────────────────┴────────────────────┘
```

| 段 | 位宽 | 容量 | 说明 |
|---|---|---|---|
| `scene`   | 16 | 65536 场景 | 高位放 scene → `Resolve(id)` 无需先知道 sceneId（修当前"解析 Entity 得先知道 sceneId"痛点） |
| `index`   | 28 | 2.68 亿槽位 | 单场景 5w 实体绰绰有余，留足回收余量 |
| `version` | 20 | 每槽复用 104 万次才回绕 | 回收 id 时 version++，旧句柄比对 version 失配即判失效 |

权威常量（`ECS_01` 逐字实现，此处定契约）：

```cpp
// Src/Common/ECS/EntityID.h
namespace MMO::ECS
{
    using EntityID = uint64;

    inline constexpr uint64 kSceneBits   = 16;
    inline constexpr uint64 kIndexBits   = 28;
    inline constexpr uint64 kVersionBits = 20;

    inline constexpr uint64 kSceneShift   = kIndexBits + kVersionBits; // 48
    inline constexpr uint64 kIndexShift   = kVersionBits;              // 20
    inline constexpr uint64 kVersionMask  = (uint64(1) << kVersionBits) - 1; // 0xFFFFF
    inline constexpr uint64 kIndexMask    = (uint64(1) << kIndexBits)   - 1; // 0xFFFFFFF
    inline constexpr uint64 kSceneMask    = (uint64(1) << kSceneBits)   - 1; // 0xFFFF

    inline constexpr EntityID kInvalidEntityID = 0; // scene=0 保留为无效

    inline constexpr EntityID MakeEntityID(uint16 scene, uint32 index, uint32 version)
    {
        return (uint64(scene)   << kSceneShift)
             | (uint64(index & kIndexMask) << kIndexShift)
             | (uint64(version) & kVersionMask);
    }
    inline constexpr uint16 SceneOf(EntityID id)   { return uint16((id >> kSceneShift) & kSceneMask); }
    inline constexpr uint32 IndexOf(EntityID id)   { return uint32((id >> kIndexShift) & kIndexMask); }
    inline constexpr uint32 VersionOf(EntityID id) { return uint32(id & kVersionMask); }
}
```

**与 EnTT 3.16.0 的关系（已核实）**：EnTT 默认 `entt::entity` 是 32 位（index 20 位 / version **仅 12 位**，`entity_mask=0xFFFFF`/`version_mask=0xFFF`）。12 位 version 在 5w 高频回收下会快速回绕别名。因此：**我们自管 index(28)+version(20)**，`entt::entity` 退化为 scene 内部的纯存储句柄，**绝不外泄**。`EntityRegistry`（`ECS_01`）维护双向映射 `uint64 ↔ (Scene*, entt::entity)`，version 校验在我们这一层做，不依赖 EnTT 的 version。

### 3.2 命名与格式（对齐 `.claude/CodingStandard.md` v2.0）

细节篇的所有代码必须遵循（本节是检查表，`08` 篇审核据此）：

- **Allman 大括号**（开括号独占一行）；4 空格缩进禁 Tab；行宽 ≤110。
- 命名空间：`MMO::ECS`（ECS 基建）/ `MMO`（World 顶层，大部分代码直接在 `MMO` 下，**不进 `MMO::World`**——已核实真实布局）。
- 类/结构体/函数：PascalCase；类成员变量 `_camelCase`；类静态常量 `kPascalCase`；结构体成员 `camelCase` 无前缀；枚举类型 `E` 前缀，枚举值 `ALL_CAPS`。
- **ID 后缀统一大写 `ID`**：`entityID`/`sceneID`/`sessionID`。缩写全大写：`ECS`/`AOI`/`AI`/`NPC`。
- 定宽整数用 `Common/Core/Types.h`（`uint8/16/32/64`、`int32` 等），**禁止裸 `int/unsigned`**；`float`/`double` 保留原名。
- **不使用 C++ 异常**（§5.4）；未用参数 `[[maybe_unused]]`，**禁止 `(void)x`**；断言用 `MASSIVE_ASSERT(cond, msg)`；日志用 `MMO::Log::{Info,Warn,Error,Debug}`。
- 注释全中文、Doxygen `/** */`；文件头模板见 `Src/Common/Network/IOContextPool.h`；生成文件头带 `生成工具:` / `来源:` / `@warning 不要手动编辑`。
- daslang 文件顶：`options gen2` + `options indenting = 4`；`require daslib/ast`（**斜杠不是点**）；`def`；`table<K; V>` 与多参 `def(a; b)` 用**分号**。

### 3.3 生成代码风格（对齐现有 `Src/World/AutoGen/*.gen.cpp`）

`ECS_05`/`ECS_08` 的 codegen 产出必须与现有 `GenMsgBindings.py` 输出同风格：文件头 doxygen；`MAKE_TYPE_FACTORY` 置顶；匿名 `namespace { using namespace das; … }`；再 `namespace MMO { Register… }`；`MMO::Proto::` 全限定；`nullptr == x` yoda 比较；中文注释；4 空格缩进。绑定手法（已核实）：

- 标量/嵌套消息字段 → `addProperty<DAS_BIND_MANAGED_PROP(x)>("x")`。
- string/bytes 字段 → `addExternProperty<DAS_BIND_FUN(acc)>(mod, lib, ".`field", "acc")->args({"msg"})`（反引号属性名，无括号访问）。
- repeated 字段 → `_size` 走 `addExternProperty`，索引访问器走 `addExtern(..., das::SideEffects::none)->args({"msg","index"})`。
- 托管结构体 → `struct XAnnotation : ManagedStructureAnnotation<T, false, false>`，ctor `: ManagedStructureAnnotation("X", ml, "Cpp::X")`。
- 枚举 → 子类 `das::Enumeration`，`external=true; cppName=...; baseType=das::Type::tInt; addI(...)`，`mod.addEnumeration(new E())` + 文件作用域 `DAS_BIND_ENUM_CAST(Cpp::E)`。
- 自由函数 → `das::addExtern<DAS_BIND_FUN(fn)>(mod, lib, "dasName", das::SideEffects::X, "CppName")->args({...})`。

---

## 4. 8 阶段模拟管线（契约，`ECS_03` 实现）

`World::Tick(dt)` 的固定阶段顺序（阶段序列写死，阶段内 system 列表可注册）：

```
 1. PreUpdate      : 处理本 tick 消息 handler 队列产生的指令
 2. ScriptLogic    : daslang [game_system]（AI/刷怪/Buff到期，分频器决定本 tick 是否跑）
 3. Movement       : C++ 移动积分（只遍历活跃集，pos += vel*dt）
 4. SpatialIndex   : C++ 更新格子索引（实体跨格才动）
 5. AOI            : C++ 增量 AOI（算 enter/leave，产生进出事件）
 6. EventDispatch  : 本 tick 累积的游戏事件出队 → 调 daslang [game_event] handler
 7. PostUpdate     : dirty 收集、死亡实体清理、休眠判定
 ── 屏障：模拟结束，组件进入只读快照窗口 ──
 8. Replicate      : [并行] 每玩家复制打包（只读，无写冲突）
```

- 阶段 1~7 全在 LogicThread 串行 → 写组件永远无锁（兑现你选的线程模型）。
- 只有阶段 8 并行且只读；`dirty` 位在**所有玩家**打包完后统一清（屏障保证不漏发）。
- **热重载安全点**：`DasLangEngine::Tick`（含 `PollReload/DoSwap`）只能在 tick 边界（阶段 7 后、下一轮阶段 1 前）调用，绝不在阶段中途。

---

## 5. 分阶段实施路线（真实可用，无妥协占位）

每阶段结束都是**可编译、可运行、可测**的完整增量，不留半成品。

| 阶段 | 目标（交付即可跑） | 涉及篇 | 依赖 |
|---|---|---|---|
| **P0 清理地基** | 删死代码/地雷（`RegisterScriptComponent` 声明、`DirtyTracker.h`）；新增 `EntityID.h` + `EntityRegistry`；`Scene` 改用 uint64；玩家 identity 归位到组件。**产出**：实体创建/销毁/解析走 uint64，version 安全。 | 01 | — |
| **P1 tick 接线** | `SceneManager` 加遍历 API；`World::Tick(dt)` + `StageScheduler`；`LogicThread` 传真 dt；`OnTick` 调 `World::Tick`。**产出**：Movement system 真正跑起来，移动服务器权威。 | 03, 02 | P0 |
| **P2 空间索引 + AOI** | `Grid` 均匀格子；增量 AOI 产生 enter/leave 事件；活跃集休眠/唤醒。**产出**：5w 实体下 AOI 不再 O(N·M)，同屏上千可查询。 | 04 | P1 |
| **P3 复制** | `REPLICATE_COMPONENT` 声明 + codegen；dirty-driven diff；带宽预算+优先级；并行打包；接 `SendRawToClient`（当前是空 stub）。**产出**：客户端看到实体状态同步。 | 05, 02 | P2 |
| **P4 脚本接缝** | `WorldBridgeModule`（Bridge_* + EntityBatch + 组件访问器）；`[game_event]`/`[game_system]` 宏；事件队列接 daslang。**产出**：daslang 能读写组件、注册玩法 system、收游戏事件。 | 06, 07 | P3 |
| **P5 收尾** | 构建 rule 全接（组件 codegen rule、AOT 覆盖新入口）；测试用例补齐；prefab 组合层。**产出**：一键构建 + 测试绿。 | 08, 01 | P4 |

每阶段的验证方法在对应篇的「迁移步骤清单」小节给出（先 `ast.parse`/编译验证 → 单测 → 集成跑）。

---

## 6. 已定决策（用户已拍板，所有细节篇遵循）

以下 5 项源自源码事实与设计取舍，已由用户确认，成为契约：

### 6.1 tick 频率 = 20ms / 50Hz（以当前源码为准）✔

- 保持 `LogicThread.h:98` 的 `kTickInterval = std::chrono::milliseconds(20)`（50Hz）不变。
- 模拟 dt 用符号常量 `kFixedDeltaTime = 0.02f`（= 20ms，秒）表达，不在系统里硬编码裸数字。
- **复制频率独立于模拟频率**：模拟 50Hz，复制打包 10~20Hz（`ECS_05` 定），近环高频、远环低频。
- （记忆里"50ms/20tps"的旧记录以此决策为准作废——本系列统一 20ms/50Hz。）

### 6.2 dt 交付 = 固定步长 accumulator ✔

- 改 `LogicThread::TickCallback` 签名为 `void(float dtSeconds)`（`ECS_03` 给出逐字改动）。
- `RunLoop` 用 accumulator 固定步长驱动：`accumulator += realElapsed; while (accumulator >= kFixedDeltaTime && steps < 3) { onTick(kFixedDeltaTime); accumulator -= kFixedDeltaTime; ++steps; }`，追帧上限 3 步防死亡螺旋，超出丢弃并告警。
- budget 的过载保护（`_currentMsgLimit` 反馈调整）保留，但与 dt 解耦——它管的是 ProcessMessages 入口，不是模拟步长。

### 6.3 EntityRegistry 归属 = `Src/Common/ECS/`（target `CommonECS`）✔

- uint64 身份是跨服务通用契约，放 Common。Social 若无场景可不实例化 scene，但 id 编解码/version 校验逻辑共享。

### 6.4 初始组件集 = 推荐集 ✔

- P0~P3 只固定：`Position`、`Velocity`、`Health`、`BattleStats`（已存在）+ 新增 `NetId{EntityID}`（entt→uint64 回查）、`GridCell{int32}`（所在格）、`VisibleSet`（玩家可见集，玩家实体专有）、`DormantTag`（休眠标记）。
- tag 统一为 `PlayerTag`/`MonsterTag`/`NpcTag`/`ItemTag`（补齐当前缺失的 `NpcTag`/`ItemTag`）。
- **废弃漂移的 `EEntityType` 枚举**：网络需要的 entity_type 字段由 tag 在复制打包时派生（`ECS_05`）。
- 其余组件（Inventory/Buff/Cooldown/Threat/Faction…）按玩法迭代再加，不进 P0（YAGNI）。

### 6.5 prefab 组合层 = daslang 表定义 ✔

- prefab 配方数据用 daslang 表写（玩法作者掌控、可热重载）。
- C++ 侧 `Bridge_Spawn(sceneID, "prefabName", x, y, z)` 查表并 emplace 组件（emplace 始终 C++ 做以保证类型固定 + 热重载安全）。落在 P5（`ECS_01` 定接口，`ECS_08` 给示例表）。

---

## 7. 与已有脚本层文档的关系

本系列（`ECS_00~08`）是 `ScriptLayer_00~05` 的**上层续作**：脚本层解决「脚本怎么编译/热重载/AOT/收消息」，本系列解决「脚本怎么操作游戏实体、C++ 怎么跑模拟与复制」。复用脚本层已定的一切：`IDasLangModuleProvider`/`IDasLangHost` 接口、`DasCommonModule`（"Common" 模块）、`.dabin`、AOT 自托管宿主、Aes256Gcm、`camel_to_msg_id` 命名约定、`[msg_handler]` 宏机制。`[game_system]`/`[game_event]` 复用 `[msg_handler]` 的 parse-time 宏基础设施（`ECS_07` 详述）。

---

> **下一步**：请逐条回复 §6 的 5 个拍板项（尤其 6.1 tick 频率）。确认后我按 `01→08` 产出全部细节篇（可照抄的主体代码 + codegen + 构建 + 测试），并派独立 agent 审核每篇的准确性/完整性/可行性。
