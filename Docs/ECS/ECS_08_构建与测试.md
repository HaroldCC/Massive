# 构建脚本与端到端测试（ECS_08）

> 汇总本系列全部构建/生成脚本变动 + 完整测试方案。
> 目标：按本系列文档实施后，`xmake build` 全量通过，端到端测试全绿。

---

## 1. 构建脚本变动汇总（全系列）

### 1.1 `Src/Common/Core/Types.h`

```cpp
// 统一模拟节奏：50ms(20Hz) → 20ms(50Hz)
inline constexpr auto kTickInterval = std::chrono::milliseconds(20);
```

> 关联：ECS_03 §6.1。`kFixedDeltaTime = 0.02f` 与 `kTickInterval = 20ms` 一致。

### 1.2 `Src/Common/ECS/xmake.lua`

```lua
--- @file xmake.lua
--- @brief CommonECS — ECS 基建（身份 + 场景 + 索引）
target("CommonECS")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("CommonCore")
    add_deps("entt", {public = true})
```

> **移除 `libDaScript` 依赖**（依赖方向铁律：CommonECS 不依赖脚本引擎）。
> 新增文件：`EntityID.h`（header-only）、`EntityRegistry.h/.cpp`、`Grid.h/.cpp`、
> `DirtyIndex.h`（header-only）、`ActiveSet.h`（header-only）。

### 1.3 `Src/World/xmake.lua`

```lua
target("WorldServer")
    set_kind("binary")
    if is_mode("release") then
        add_rules("Rules.das_aot", {service = "world", entry = "World/main.das"})
        add_includedirs("$(projectdir)/Src")
    end
    -- 删除的 Component/System/Handler/Scene 目录重建后自动纳入
    add_files("**.cpp", {excludes = {
        "DasModule/WorldScriptModule.cpp",
        "AutoGen/*.gen.cpp"
    }})
    add_headerfiles("*.h")
    add_deps(
        "CommonCore", "CommonDB", "CommonNetwork", "CommonQueue",
        "CommonCrypto", "CommonECS", "CommonTimer", "CommonConfig",
        "CommonLog", "Proto", "ScriptEngine", "ProtoScriptModule"
    )
    if is_mode("release") then
        add_deps("AotGen")
    end
    add_deps("asio", {public = true})
```

> **无实质改动**——`add_files("**.cpp")` 通配自动覆盖新目录。

### 1.4 `Src/World/DasModule/xmake.lua`

```lua
-- 无改动：Bridge_* 绑定直接在 WorldScriptModule.cpp 内扩展
-- 若新增独立绑定文件（如 WorldBridge.cpp），add_files 通配自动纳入
```

### 1.5 `Src/ScriptEngine/xmake.lua`

```lua
-- 无改动：GameEventBus.h（header-only）由 add_headerfiles 自动纳入
```

### 1.6 `Src/Proto/Replicate.proto`（决策 2：直接改类型）

```protobuf
// Replicate.proto —— entity_id 直接改 uint64（无兼容字段，开发阶段无旧客户端）
message EntitySpawnNtf
{
    uint64 entity_id   = 1;  // uint32 → uint64
    uint32 entity_type = 2;
    PositionDelta position = 3;
    int32  hp_current = 4;
    int32  hp_max     = 5;
}
message EntityUpdateNtf
{
    uint64 entity_id = 1;
    optional PositionDelta position    = 2;
    optional int32         hp_current  = 3;
    optional bool          is_dead     = 4;
    optional bool          is_in_combat = 5;
}
message EntityDespawnNtf
{
    uint64 entity_id = 1;
}
```

> 连带：`Login.proto` 的 `LoginEnterWorldRsp.player_id` 也直接改 `uint64`。
> proto 变更自动触发 `proto_gen` rule 重新生成（xmake mtime 检测）。`MsgID.proto` 不变。

### 1.6b `Src/Proto/GameEvent.proto`（决策 3：新建）

```protobuf
// GameEvent.proto — 游戏事件定义（C++ 产出 → daslang [game_event] 消费）
// 内容见 ECS_06 §3.1（EGameEventType 枚举 + 各 *Event 消息）
```

### 1.7 生成脚本

| 生成器 | 触发 | 改动 |
|---|---|---|
| `proto_msgid` / `proto_gen`（`Src/Proto/xmake.lua`） | `.proto` mtime 变化 | `Replicate.proto` 改类型 + `GameEvent.proto` 新建 → 自动重生成 |
| `gen_emsgid_bind`（`Src/ScriptEngine/xmake.lua`） | `MsgID.proto` 变化 | **无改动**（MsgID 不变） |
| `gen_msg_bindings`（`Src/World/DasModule/xmake.lua`） | `.proto` mtime 变化 | `*.gen.cpp` 重新生成 + **新增 `GameEventBindings.gen.{h,cpp}`**（事件类型绑定 + `Emit<Type>` 工厂 + `RegisterAllGameEventBindings`） |
| `Rules.das_aot`（根 `xmake.lua`） | release 构建 | 无改动（入口 `World/main.das`） |

### 1.8 `Config/world.toml`

```toml
[world]
id = 1
max_players = 10000
persistent_scenes = ["1"]
```

> `persistent_scenes` 语义：场景 ID 列表（参数由 `World` 默认值管理，后续按需 toml 扩展）。

---

## 2. 完整测试方案

### 2.1 单元测试（`Tools/Tests/` 或临时 test target）

| 测试 | 覆盖 | 命令 |
|---|---|---|
| `EntityRegistryTest` | Create/Destroy/IsValid/version 递增/回绕 | 临时 main |
| `DirtyIndexTest` | Mark 去重/Drain 清空/越界安全 | 临时 main |
| `GridTest` | Insert/Update 跨格/QueryRadius 去重/Clear | 临时 main |
| `SystemAOITest` | enter/leave 事件流/Y 轴过滤/静止无事件 | 临时 main |
| `SystemMovementTest` | Position += Velocity*dt/排除 DeadTag | 临时 main |

> 推荐建 `Tools/Tests/ecs_test.cpp` 独立 target（见 §2.3），或临时在 WorldServer 内加
> `--selftest` 参数跑内建测试。

### 2.2 集成测试（端到端）

```powershell
# 1. 启动服务栈
python Tools/ServerCtl.py up

# 2. TestClient 压测
#    多客户端登录 → EnterWorld → 移动
#    预期：每客户端收到 EntityReplicateNtf（spawn 其他玩家 + update 位置）

# 3. 热重载测试（开发期）
#    改 main.das → 保存 → 日志 "Script hot-reloaded"
#    玩家数据不丢

# 4. 热重载测试（生产期）
#    Release 构建 → 生成 dasbin → 运行期下发 → 热更生效

# 5. AOT 验证
#    Release 日志确认 fn->aot == true
```

### 2.3 测试 target（推荐，`Tools/Tests/xmake.lua`）

```lua
target("EcsTests")
    set_kind("binary")
    add_files("*.cpp")
    add_deps("CommonECS", "CommonCore", "CommonLog")
    add_deps("entt", {public = true})
```

```cpp
// Tools/Tests/ecs_test.cpp
#include "Common/ECS/EntityRegistry.h"
#include "Common/ECS/DirtyIndex.h"
#include "Common/ECS/Grid.h"
#include "Common/ECS/Scene.h"
#include <cstdio>
#include <cassert>

using namespace MMO::ECS;

int main()
{
    // ── EntityRegistry 测试 ──
    {
        EntityRegistry reg(1);
        auto id1 = reg.Create();
        assert(IsValidEntity(id1));
        assert(reg.IsValid(id1));
        assert(reg.Destroy(id1));
        assert(!reg.IsValid(id1));          // 销毁后失效
        auto id2 = reg.Create();            // 复用槽位
        assert(IndexOf(id2) == IndexOf(id1));
        assert(VersionOf(id2) != VersionOf(id1)); // version 递增
        assert(SceneOf(id2) == 1);
    }

    // ── DirtyIndex 测试 ──
    {
        DirtyIndex<int> di;
        di.Mark(5); di.Mark(5); di.Mark(7);   // 5 重复标记
        auto drained = di.Drain();
        assert(drained.size() == 2);          // 去重后 5,7
        assert(di.Count() == 0);              // Drain 清空
        di.Mark(9);
        assert(di.Count() == 1);
        di.Mark(1000000);                     // 越界 resize 安全
        assert(di.Count() == 2);
    }

    // ── Grid 测试 ──
    {
        Grid grid(10.0f);
        grid.Insert(1, 5.0f, 5.0f);
        grid.Insert(2, 95.0f, 5.0f);          // cell 9,0
        grid.Insert(3, 5.0f, 95.0f);          // cell 0,9

        std::vector<uint32> out;
        grid.QueryRadius(0.0f, 0.0f, 15.0f, out);
        assert(out.size() == 1 && out[0] == 1); // 只有实体 1

        grid.Update(2, 15.0f, 5.0f);          // 跨格到 cell 1,0
        grid.QueryRadius(0.0f, 0.0f, 30.0f, out);
        assert(out.size() == 2);              // 1 + 2

        grid.Remove(1);
        grid.QueryRadius(0.0f, 0.0f, 30.0f, out);
        assert(out.size() == 1 && out[0] == 2);
    }

    // ── Scene 测试 ──
    {
        Scene scene(1);
        auto id = scene.CreateEntity();
        assert(scene.IsValid(id));
        scene.EmplaceComponent<int>(id, 42);
        assert(scene.GetComponent<int>(id) == 42);
        assert(scene.HasComponent<int>(id));
        assert(scene.DestroyEntity(id));
        assert(!scene.IsValid(id));
    }

    printf("All ECS tests PASSED\n");
    return 0;
}
```

> `Tools/Tests/xmake.lua` 需在根 `xmake.lua` 的 `includes` 里加
> `includes("Tools/Tests/xmake.lua")`（或暂不纳入，独立构建）。

---

## 3. 实施顺序与验收（严格按序）

| 阶段 | 文档 | 验收标准 |
|---|---|---|
| **P0** | ECS_01 身份系统 | WorldServer 构建通过；EntityRegistry 单测绿 |
| **P1** | ECS_02 组件集 | 构建通过；DirtyIndex 单测绿 |
| **P2** | ECS_03 阶段调度 | 构建通过；Movement 单测绿；OnTick(float) 生效 |
| **P3** | ECS_04 空间索引 | 构建通过；Grid/AOI 单测绿 |
| **P4** | ECS_05 网络复制 | 构建通过；TestClient 收到 spawn/update/despawn |
| **P5** | ECS_06 脚本接缝 | 构建通过；`[game_system]`/`[game_event]` 冒烟绿 |
| **P6** | ECS_07 热重载+AOT | 双通道热更绿；IV 随机；AOT 生效 |
| **P7** | ECS_08 全量测试 | 全量构建 + 端到端绿 |

**关键决策点（已拍板，见 ECS_09）**：

1. **P3 场景粒度**：方案 C——串行模拟 + 复制并行（先串行正确，再并行提速）
2. **P4 协议兼容**：直接改字段类型 `uint32 → uint64`（开发阶段无旧客户端）
3. **P5 事件参数**：类型化事件（每事件一个 struct）+ 生成器自动绑定（`GameEvent.proto` + `GameEventBindings.gen.*`）

---

## 4. 风险与缓解

| 风险 | 缓解 |
|---|---|
| EnTT 3.16 API 差异 | 所有 EnTT 调用对照 `ThirdParty/entt/src` 源码核实（ECS_01 已核） |
| `float3` 绑定跨语言 | 参考既有 `BattleStats` 绑定（`das::cast<float3>`），先冒烟验证 |
| AOT aotHash 漂移 | `MakeScriptPolicies` 两端逐字段一致；`fail_on_no_aot=false` 保运行 |
| dasbin IV 破坏兼容 | `formatVersion` 递增；旧 dasbin 失效重新生成 |
| 多场景 context 共享状态 | `module shared` 修饰符按场景隔离需求选择 |
| 热重载丢脚本状态 | 铁律 1：数据在 EnTT；脚本状态 `[init]` 重建 |
| 并行打包竞态 | `results` 预分配槽位（任务 i 独占写 `results[i]`），避免多 worker 共享写 |
| 事件生成器复杂度 | `GameEvent.proto` 保持简单字段（uint64/int32/uint32），不引入 repeated/map |

---

## 5. 最终交付检查清单（2026-08-04 更新）

- [x] `xmake build`（debug + release）零错误
- [x] `xmake f -m release && xmake build` 触发 AOT 生成无错误（`World_main.das.cpp` 编译通过）
- [x] `Tools/Tests/EcsTests` 全部断言通过（EntityID/EntityRegistry/DirtyIndex/Grid/Scene）
- [ ] TestClient 端到端：登录 → 进世界 → 移动 → 收到复制（需完整服务栈）
- [ ] 开发期热重载：改脚本生效，玩家数据不丢（机制完整，FileWatcher 边界待验证）
- [x] 生产期热更：Release 构建 + AOT 链接正常（`fail_on_no_aot=false` 回退就绪）
- [x] `Replicate.pb.h` 的 `entity_id` 为 uint64（决策 2）
- [x] `LoginEnterWorldRsp.player_id` 为 uint64（决策 2）
- [x] `GameEvent.pb.h` + `GameEventBindings.gen.*` 生成成功（决策 3）
- [x] `[game_event]`/`[game_system]` 宏编译 + 运行验证通过（`ai_tick` 每秒跑）
- [x] Bridge 实体操作（Create/SetPosition/GetPosition）运行验证通过
- [x] dasbin IV 随机化（RAND_bytes）
- [ ] 无 `ScriptComponentStorage`/`DirtyTracker`/旧 `Entity` 残留引用（残留检查见 ECS_10）
- [ ] `[game_event] def Xxx(ev : XxxEvent)` 编译通过且运行分发正确（决策 3）
- [ ] `Config/world.toml` 语义清晰
- [ ] 无 `ScriptComponentStorage`/`DirtyTracker`/旧 `Entity` 残留引用
