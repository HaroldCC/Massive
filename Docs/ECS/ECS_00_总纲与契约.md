# ECS 实体层重构 — 总纲与权威契约（ECS_00）

> 状态：**已拍板的设计**（非计划草案）
> 本系列文档是**可照抄实现**的完整设计，每篇给出可编译的完整代码、构建脚本变动、测试步骤。
> 执行顺序严格按文档编号：`00 → 01 → ... → 08`，每篇交付后**必须**通过该篇的"验证步骤"再进入下一篇。

---

## 1. 背景与现状（已核实）

### 1.1 已删除的旧实体层（本次重构清理）

以下文件为旧 ECS 计划产物（死代码或绑定旧契约），**已删除**：

| 文件 | 删除原因 |
|---|---|
| `Src/Common/ECS/Entity.h` | 旧 `Entity{sceneId, entityId}` 结构体，无 version 校验 |
| `Src/Common/ECS/Scene.h/.cpp` | 含 `RegisterScriptComponent` 声明无定义（链接地雷） |
| `Src/Common/ECS/DirtyTracker.h` | 零引用死代码 |
| `Src/World/Component/*`（6 个组件） | 旧组件集，无 dirty/所有权模型 |
| `Src/World/System/System.h/.cpp` | `RunCPPSystems/SystemMovement/SystemAOI` 零调用点 |
| `Src/World/Handler/EnterWorldHandler.*` | 绑定旧 `Entity` 结构 |
| `Src/World/Handler/MoveHandler.*` | 绑定旧 `WorldSession::entity` |
| `Src/World/SceneManager.*` | 绑定旧 `Scene` |
| `Script/Tests/Test_EntitiesInRadius.das` | 依赖已删的 `massive` 模块 |

### 1.2 保留的既有基础设施（不重写）

| 模块 | 说明 |
|---|---|
| `Src/World/WorldServer.cpp/.h` | 网络/会话/脚本引擎接线（已清理对旧 ECS 的引用） |
| `Src/World/WorldSession.h` | 会话结构（已移除 `Entity` 字段，保留 `sessionID/accountID/crypto/inbox`） |
| `Src/World/LogicThread.*` | 单线程逻辑循环（**待改**：dt 传递） |
| `Src/ScriptEngine/*` | DasLang 引擎（编译/热重载/dasbin 序列化） |
| `Src/World/DasModule/*` | World 专用 das 模块 + Proto 绑定生成 |
| `Script/Common/MsgHandlerRegistry.das` | `[msg_handler]` 宏模块 |
| `Script/World/main.das` | 脚本入口（**待改**：接 ECS 事件） |
| `Tools/AotGen/*` + `Rules.das_aot` | AOT 生成宿主（已接线） |

### 1.3 设计前提（已拍板）

1. **无 MVP 阶段**——按依赖顺序一次到位，每篇交付完整可用。
2. **热重载双通道**：
   - 开发期：改 `.das` → 源码重编译 → 新 Program + 每场景 context 重建
   - 生产期：下发加密 `.dasbin` → `RequestReload(path)` → 同流程
3. **发布不带源码**：Release 全量 AOT（`policies.aot=true`）+ 加密 dasbin；`fail_on_no_aot=false` 保热更回退。

---

## 2. 架构铁律（不可违背）

> 这三条是 5w 实体/场景 + daslang 玩法 + 双通道热重载三个约束交汇后的唯一可行解。
> 任何实现若与之冲突，以本节为准。

### 铁律 1 — 组件数据永远只活在 C++/EnTT，绝不进 das context/heap

脚本只持有 `uint64 EntityID`（纯整数）和**临时值拷贝**。热重载 `DoSwap` 时
`DasLangEngine::_scriptImage = std::move(img)` 直接丢弃旧 Context——EnTT 组件数据
原封不动（零丢失），脚本侧注册表/值拷贝随旧 context 销毁后由新 context 的 `[init]` 重建。

**依据**：`DasEngine.cpp:284` `DoSwap` 的实现；DECS 组件数据在 das heap（`array<uint8>`），
context swap 必丢——这是 DECS 出局的根本原因。

### 铁律 2 — 脚本调用量 ∝ 事件数，绝不 ∝ 实体数 × tick

5w × 50Hz 的每实体每帧脚本调用 = 250 万次/秒跨 AOT/解释器边界，必死。
因此：
- C++ 跑所有 O(实体数) 的热循环（移动积分、格子索引、增量 AOI、复制 diff）
- daslang 只跑**离散事件**（伤害/死亡/拾取/进出 AOI/技能命中）和**分频错峰**的低频决策（AI/刷怪/Buff 到期）

### 铁律 3 — 每场景 1 registry + 1 context + 1 模拟线程

```
进程 WorldServer
├─ 场景 1: entt::registry#1 + das::Context#1 + 模拟线程#1
├─ 场景 2: entt::registry#2 + das::Context#2 + 模拟线程#2
└─ 共享: das::Program（编译产物 + AOT 代码，全局唯一）
```

- 场景内串行（确定性 + 无锁）；场景间并行（多核利用）
- das context 是 per-scene 克隆（`Program::cloneContext`），数据隔离、代码共享
- 跨场景交互（传送/组队）走进程内事件队列，不走跨进程 RPC

---

## 3. 分层架构

```
┌───────────────────────────────────────────────────────────────┐
│ daslang 玩法层（热重载/AOT 单元）— Script/World/                │
│   · [msg_handler]  网络消息 → 脚本（已存在）                    │
│   · [game_event]   游戏事件 → 脚本（本篇系列新增）               │
│   · [game_system]  低频决策 system → 脚本（本篇系列新增）        │
│   —— 调用量 ∝ 事件数（铁律 2）——                                │
└──────────────▲──────────────────────────────┬────────────────┘
    Bridge_* extern（读写组件/查询/生成）   │ 事件出队（C++ → 脚本）
               │                             ▼
┌──────────────┴─────────────────────────────────────────────────┐
│ C++ 物理层（数据唯一真相）— Src/World/ + Src/Common/ECS/         │
│   EntityRegistry（uint64 ↔ entt::entity，index+version 自管）   │
│   Scene（entt::registry 持有者，组件唯一所有者）                 │
│   StageScheduler: PreUpdate→ScriptLogic→Movement→SpatialIndex   │
│                   →AOI→EventDispatch→PostUpdate→[并行]Replicate  │
│   Grid（均匀格子空间索引）· ActiveSet（活跃集）· DirtyIndex       │
└──────────────┬──────────────────────────────────────────────────┘
               │ 只读快照（模拟结束后）
               ▼   线程池并行，每玩家一个打包任务
        复制打包 → 带宽预算+优先级 → Aes256Gcm → Gate
```

**依赖方向铁律**：`Src/ScriptEngine/` 是服务无关下层库，**不得反向依赖 World/Social**。

| 组件 | 归属目录 | 理由 |
|---|---|---|
| `EntityID` / `EntityRegistry` / `Scene` / `Grid` / `DirtyIndex` / `ActiveSet` | `Src/Common/ECS/`（target `CommonECS`） | 跨服务通用的 ECS 基建 |
| 具体组件（`Position`/`Health`/…） | `Src/World/Component/` | World 专用玩法数据 |
| `StageScheduler` / `World` / 业务 System | `Src/World/` | World 模拟管线 |

---

## 4. 权威契约（全系列唯一真相源）

### 4.1 uint64 EntityID 位布局（强类型）

```
   63          48 47                      20 19                  0
  ┌──────────────┬──────────────────────────┬────────────────────┐
  │  scene (16)  │        index (28)         │    version (20)    │
  └──────────────┴──────────────────────────┴────────────────────┘
```

- **scene (16位)**：场景 ID，0 保留为无效
- **index (28位)**：EntityRegistry 内数组下标（2.68 亿，足够 5w/场景）
- **version (20位)**：回收时递增，旧句柄比对即失效（防悬垂）
- 高位放 scene → `Resolve(id)` 无需先知道 sceneID

**强类型**（决策 4，2026-08-03）：`EntityID` 是 `struct EntityID { uint64 raw; }` 而非裸
`uint64`——与普通整数强区分（类比 EnTT `enum class entity`），杜绝 entityID 被当
sessionID/accountID 用。详见 `ECS_09` 补充决策 4。

**跨边界规则**：
| 边界 | 规则 |
|---|---|
| C++ 内部 | 一律 `ECS::EntityID` 强类型 |
| 网络/DB/脚本 | `static_cast<uint64>(eid)`（隐式）进出；protobuf 字段 `uint64` |
| 日志 | `Log::Info("{}", eid)`（formatter 特化） |
| 容器键 | `unordered_map<EntityID, T>`（hash 特化） |

### 4.2 固定常量（跨 C++/daslang/网络/DB 唯一）

| 常量 | 值 | 说明 |
|---|---|---|
| `kFixedDeltaTime` | `0.02f`（20ms） | 模拟步长唯一真相源 |
| `kMaxCatchUpSteps` | `3` | 卡顿追帧上限（防死亡螺旋） |
| `kMaxElapsed` | `50ms` | 单帧最大真实耗时 clamp |
| `kGridCellSize` | `10.0f` | 空间索引格子边长（世界单位） |
| `kViewRadiusXZ` | `100.0f` | 玩家视野水平半径 |
| `kViewRadiusY` | `15.0f` | 玩家视野垂直半径 |
| `kReplicateBudgetBytes` | `64KB` | 单玩家单帧复制带宽预算 |
| `kReplicatePrioritySteps` | `3` | 复制优先级分档 |

### 4.3 目录与命名

- **新文件一律放在** `Src/Common/ECS/`（跨服务）或 `Src/World/`（World 专用）
- C++ 类型：PascalCase（`EntityRegistry`/`StageScheduler`）
- C++ 函数：PascalCase（`Resolve`/`CreateEntity`）
- 成员变量：`_camelCase`（`_registry`/`_versionPool`）
- das 绑定函数：`Bridge_*` 前缀（`Bridge_EntityGetPosition`）
- das 组件访问宏：`[game_event]` / `[game_system]`

### 4.4 阶段管线（8 阶段，顺序固定）

```
PreUpdate → ScriptLogic → Movement → SpatialIndex → AOI
         → EventDispatch → PostUpdate → [并行] Replicate
```

| 阶段 | 内容 | 执行者 |
|---|---|---|
| `PreUpdate` | 消息入队处理、EnterWorld、断线清理 | C++ |
| `ScriptLogic` | 低频决策（AI/技能/Buff）→ 事件出队 | daslang |
| `Movement` | 移动积分 `Position += Velocity * dt` | C++（EnTT view） |
| `SpatialIndex` | 更新格子索引（增量） | C++（Grid） |
| `AOI` | 增量 AOI，产出入/出事件 | C++（Grid query） |
| `EventDispatch` | 事件队列 → daslang `[game_event]` | C++ → daslang |
| `PostUpdate` | 死亡清理、DirtyIndex 汇总 | C++ |
| `Replicate` | 只读快照 → 并行打包 → 加密 → Gate | C++（线程池） |

---

## 5. 交付物总览（按依赖顺序）

| # | 文档 | 交付内容 | 依赖 |
|---|---|---|---|
| 01 | 身份系统 | `EntityID.h`、`EntityRegistry`、`Scene` 重构、`WorldSession` 归位 | 00 |
| 02 | 组件集 | `Component/` 组件声明、`DirtyIndex`、所有权模型 | 01 |
| 03 | 阶段调度 | `LogicThread` 固定步长、`StageScheduler`、`World::Tick`、`SystemMovement` 重写 | 02 |
| 04 | 空间索引 | `Grid` 均匀格子、增量 AOI、`ActiveSet` | 03 |
| 05 | 网络复制 | 复制系统：dirty-driven + 带宽预算 + 并行打包 | 04 |
| 06 | 脚本接缝 | `Bridge_*` extern、事件队列、`EntityBatch`、`[game_event]`/`[game_system]` 宏 | 03/04 |
| 07 | 热重载+AOT | 多 context clone/swap、dasbin IV 修复、补丁校验 | 06 |
| 08 | 构建+测试 | xmake rule、codegen 脚本、端到端测试 | 全部 |

---

## 6. 已拍板的决策（本系列不重复讨论）

1. **数据归属**：C++ EnTT 拥有全部组件数据（铁律 1）
2. **脚本职责**：daslang 只跑事件 + 低频决策（铁律 2）
3. **场景模型**：1 进程 N 场景，每场景 1 registry + 1 context + 1 线程（铁律 3）
4. **dt 传递**：`LogicThread` 改 `TickCallback = std::function<void(float)>`，固定步长 accumulator
5. **进程粒度**：1 进程 N 场景（跨地图交互走进程内事件）
6. **热更新通道**：开发期源码 + 生产期 dasbin 双通道
7. **AOT 策略**：Release 全量 AOT，`fail_on_no_aot=false` 回退解释器

---

## 7. 安全约束（贯穿实现）

1. `DasSerializer.cpp` IV 派生修复（当前 `blobLen` 派生，同长度脚本 IV 重用）——**必须在 07 篇落地**
2. dasbin 加密：Aes256Gcm（AEAD），密钥 hex 内嵌配置（门槛级，后续可密钥服务器下发）
3. aotHash 匹配失败静默回退解释器（`fail_on_no_aot=false`），bring-up 期临时 `true` + 覆盖率日志
