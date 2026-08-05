# 实现审查与缺口记录（ECS_10）

> 本文档记录 2026-08-04 对已实现代码（ECS_00~05）的审查结果：
> 已修复的缺口、当前可用状态、后续待办。
> **目的**：让"下一步做什么"一目了然，避免重复排查。

---

## 1. 审查结论（ECS_00~05 实现状态）

### 1.1 已完成且可编译（构建验证通过）

| 模块 | 状态 | 说明 |
|---|---|---|
| `EntityID.h` | ✅ | 强类型 struct + `EntityIndex` + hash/formatter 特化 |
| `EntityRegistry.h/.cpp` | ✅ | index+version 自管，Create/Destroy/IsValid/Resolve/ToEntityID |
| `Scene.h/.cpp` | ✅ | 持有 `EntityRegistry` + `entt::registry` + `Grid` + `ActiveSet` |
| `Grid.h/.cpp` | ✅ | 均匀格子，Insert/Update(3参)/Remove/QueryRadius |
| `DirtyIndex.h` | ✅ | 带去重的脏标记，键为 `EntityIndex` |
| `ActiveSet.h` | ✅ | 活跃实体集，键为 `EntityIndex` |
| `StageScheduler.h/.cpp` | ✅ | 8 阶段调度，RegisterSystem/RunStage/RunAllStage |
| `World.h/.cpp` | ✅ | Movement + AOI 阶段已接线，持有 AOI 状态 |
| `SystemMovement.h/.cpp` | ✅ | Position += Velocity*dt，排除 Dead/Dormant |
| `SystemAOI.h/.cpp` | ✅ | 增量 AOI，产出 enter/leave（EntityIndex） |
| `SystemReplicate.h/.cpp` | ✅ | PackPlayerReplicate（Spawn/Update/Despawn 全实现）+ 并行调度 |
| `WorldServer.h/.cpp` | ✅ | InitWorlds + `_replicateSystem` + `SendEncrypted` + OnTick 接线 |
| `EnterWorldHandler.cpp` | ✅ | 玩家实体组件（PlayerConnection/Position/PlayerTag/Health）+ 进格子 |

### 1.2 本次审查修复的缺口（已落地）

| # | 缺口 | 修复 |
|---|---|---|
| 1 | `SendEncrypted` 未定义（复制发送断） | 实现（复用加密+PacketHeader+SendToGate 逻辑） |
| 2 | `ReplicateScheduler::Update` 无调用点 | `OnTick` 接入（消费 World 的 AOI 状态） |
| 3 | `SystemAOI` 未接入 World | `World::Init` 注册到 AOI 阶段 |
| 4 | `PackPlayerReplicate` Spawn/Update 是 `// todo` | 补全（PackSpawnEntity/PackUpdateEntity/PackDespawnEntity） |
| 5 | `Scene` 不持有 Grid/ActiveSet | 加 `_grid`/`_activeSet` 成员 + Getter |
| 6 | 并行打包 `results` 竞态（emplace_back 多线程写） | 预分配槽位 `results[i]` 独占写 |
| 7 | `std::jthread` 兼容性 | 改 `std::thread` |
| 8 | `_aoiState/_aoiEnters/_aoiLeaves` 在 WorldServer 冗余 | 删除（World 已持有） |
| 9 | `EEntityType` 重复定义（建了 EntityType.h） | 删除文件，用 `Types.h` 的 `EEntityType` |
| 10 | `EnterWorldHandler` 未给玩家加组件 | 加 PlayerConnection/Position/PlayerTag/Health + 进格子 |
| 11 | include 冗余/重复 | 清理 |
| 12 | `entityIndex` 裸 uint32 语义混淆 | 引入 `EntityIndex` 强类型（决策 5） |

### 1.3 当前数据流（已接通）

```
EnterWorld（玩家）→ CreateEntity + PlayerConnection/Position/PlayerTag/Health + Grid.Insert
World::Tick(dt)
  → Movement 阶段：SystemMovement（Position += Velocity*dt）
  → AOI 阶段：SystemAOI（Grid.Update + QueryRadius → enter/leave → _aoiState/_aoiEnters/_aoiLeaves）
WorldServer::OnTick
  → ProcessUnroutedMessages / ProcessControlMessages
  → for world: world->Tick(dt)
  → _replicateSystem->Update(world.GetAoiState(), world.GetAoiEnters(), world.GetAoiLeaves())
      → PackPlayerReplicate（Spawn/Update/Despawn → EntityReplicateNtf 字节）
      → SendEncrypted（加密 + PacketHeader + SendToGate）
  → 过载保护
```

---

## 2. 后续待办（ECS_06~08 实施状态）

### 2.1 ECS_06 脚本接缝（✅ 已交付 2026-08-04）

| 项 | 状态 | 说明 |
|---|---|---|
| `GameEvent.proto` | ✅ | 8 个类型化事件（EntityDamaged 等） |
| `GameEventBus.h` | ✅ | 事件总线（修复 `_payload` 大括号窄化 bug） |
| `GenMsgBindings.py` 扩展 | ✅ | 生成 `GameEventBindings.gen.{h,cpp}` |
| `[game_event]` 宏 | ✅ | `Script/Common/GameEventRegistry.das` 类型化分发 |
| `[game_system]` 宏 | ✅ | `Script/Common/GameSystemRegistry.das` interval 错峰 |
| `Bridge_*` extern | ✅ | `WorldBridge.{h,cpp}` 实体创建/位置/血量/标签 |
| `WorldScriptModule.cpp` 扩展 | ✅ | 注册事件绑定 + Bridge |
| `Scene::CreateEntity` 双 registry 同步 | ✅ | **关键修复**：`_entt.create` 用同一 index |

**运行验证**：`ai_tick` 每秒跑，Bridge `EntityCreate`/`SetPosition`/`GetPosition` 全通（位置 (1.5,2.5,3.5) 正确回读）。

### 2.2 ECS_07 热重载 + AOT

| 项 | 状态 | 说明 |
|---|---|---|
| 多 context（每场景） | ⏸ 单场景等价 | 当前单场景 `GetScriptContext()` 已够用；多场景需 `RegisterSceneContext` |
| `DoSwap` 重建 context | ✅ 已有 | `SimulateImage` 新建 ctx + `_scriptImage` 替换（EnTT 数据不动） |
| dasbin IV 修复 | ✅ 已修 | `DasSerializer.cpp` 随机 IV（RAND_bytes）替代 blobLen 派生 |
| AOT 验证 | ⬜ 未验证 | release 构建确认 `fn->aot`（AotGen 已生成 .das.cpp） |

### 2.3 ECS_08 构建与测试

| 项 | 状态 | 说明 |
|---|---|---|
| `Tools/Tests/EcsTests` | ⬜ 未建 | 单元测试 target |
| `GameEvent.proto` 生成规则 | ✅ | proto_gen 自动覆盖（已验证） |
| 端到端验证 | ⏸ 部分 | WorldServer 启动 + 脚本冒烟已通；TestClient 复制未测 |

---

## 3. 已知设计注意点（编码时注意）

1. **`Scene::GetDirtyIndex<T>()` 用函数内 static**——每组件类型一份，跨 Scene 共享。
   多场景时脏标记跨场景混用（`Mark` 用的是 EnTT index，不同场景 index 冲突）！
   **若多场景并行，必须改为 Scene 实例持有**（`std::unordered_map<std::type_index, ...>` 或
   EnTT 的 `storage` 内嵌）。当前单场景无碍，多场景前必修。
2. **`Grid` 默认 10 世界单位/格**——`Scene::_grid` 构造用默认值。若需按场景配置，
   `Scene` 构造函数应接收 `gridCellSize` 参数（当前未传）。
3. **`ReplicateScheduler` 的 `_workerCount` 默认 4**——单场景串行模拟下，复制并行有收益；
   但 `results` 槽位数组每帧分配（`std::vector<TaskResult>(taskCount)`），玩家多时分配开销
   值得优化（成员复用）。
4. **`SendEncrypted` 在 LogicThread 调用**（OnTick 内）——与 `SendToClient` 一致，安全。
   若将来复制并行线程直接发，需考虑 `_sessions` 锁。
5. **`EnterWorldHandler` 里 `EntityIndex(static_cast<uint32>(entityID))`**——这里取的是
   `EntityID` 的**低 32 位**作 index。`EntityRegistry::Create` 的 index 从 0 递增，低 32 位
   恰好是 index（version 在高位）——**成立**，但更稳妥应显式 `EntityIndex(IndexOf(entityID))`。
6. **float3 跨语言**：das 的 `float3` 在 C++ interop 是 `vec4f`（`__m128`），绑定用
   `addExternEx<float3(...), DAS_BIND_FUN(fn)>` 显式签名 + `v_make_vec4f`/`v_extract_x`。
7. **双 registry 同步**：`Scene::CreateEntity` 必须在 `_entt.create()` 用同一 index 创建
   EnTT 实体（只 `EntityRegistry.Create()` 会导致 `get_or_emplace` 崩）——**已修复**。

---

## 4. 建议的下一步实施顺序

1. **ECS_07 热重载**（✅ dasbin IV 已修；多 context 单场景等价）——生产可用性
2. **ECS_08 测试**（EcsTests + 端到端）——验证前面全部
3. **Bridge 扩展**（BattleStats/实体空间查询/事件 Emit 注入）——玩法层

> 当前状态：ECS_00~07 核心全部落地，WorldServer 可启动运行，脚本（`[game_event]`/`[game_system]`/
> Bridge）全链路验证通过。剩余：ECS_08 测试基建 + 多场景并行。
