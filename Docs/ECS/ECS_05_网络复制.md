# 网络复制系统（ECS_05）

> 契约见 `ECS_00` §4.2/§4.4。本篇给出**可照抄实现**：
> 复制打包（Spawn/Update/Despawn）→ 带宽预算 → 优先级 → 并行打包 → 加密发送。
> 交付后：客户端能看到实体进入/离开视野 + 位置/血量更新。

---

## 1. 设计原则

1. **dirty-driven**：组件写时 `MarkDirty`（ECS_02），复制帧末 Drain 脏集打包。
2. **AOI 驱动 spawn/despawn**：进视野 → Spawn，出视野 → Despawn（ECS_04 事件流）。
3. **带宽预算**：单玩家单帧 `kReplicateBudgetBytes = 64KB`，超预算按优先级截断。
4. **只读快照 + 并行打包**：模拟结束后，从 registry 读只读快照，线程池每玩家一个打包任务。
5. **客户端权威**：位置全量（每活跃玩家每帧），血量/标签 dirty 驱动。

---

## 2. 复制消息（复用现有 `Replicate.proto`）

`EntityReplicateNtf`（spawns/updates/despawns）已存在——**复用**。

> **决策 2 定稿（ECS_09）**：开发阶段无旧客户端——`entity_id` **直接改 uint64**，不加兼容字段。

```protobuf
// Replicate.proto（决策 2：直接改类型）
message EntitySpawnNtf
{
    uint64 entity_id   = 1;  // uint32 → uint64（EntityID 契约）
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

> **连带**：`Login.proto` 的 `LoginEnterWorldRsp.player_id` 也直接改 `uint64`；
> `MsgID.proto` 不变（消息 ID 不变）。
> **注意**：复制打包时必须用 `Scene::ToEntityID(e)` 拿完整 uint64 EntityID，
> 不能只传 EnTT `to_integral` 的 uint32（场景位/版本位会丢）。

---

## 3. `ReplicateSystem` — 复制打包

```cpp
// Src/World/System/SystemReplicate.h
#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 复制打包——每帧为每个玩家生成 EntityReplicateNtf
     *
     * 输入：AOI 事件（enter/leave）+ 脏组件 + 活跃集
     * 输出：per-session 的序列化字节（加密在 WorldServer 层做）
     */
    class ReplicateSystem
    {
    public:
        using SendFn = std::function<void(uint32 sessionID, const uint8 *data, size_t len)>;

        ReplicateSystem(entt::registry &reg, SendFn sendFn)
            : _reg(reg), _sendFn(std::move(sendFn))
        {
        }

        /**
         * @brief 每帧复制
         * @param dt         固定步长
         * @param prevState  玩家上帧可见集（AOI 快照，输入）
         * @param enters     本帧 enter 事件（observerIdx, entityIdx）
         * @param leaves     本帧 leave 事件
         */
        void Update(float dt,
                    const std::unordered_map<uint32, std::unordered_set<uint32>> &prevState,
                    const std::vector<std::pair<uint32, uint32>> &enters,
                    const std::vector<std::pair<uint32, uint32>> &leaves);

    private:
        void PackSpawn(uint32 observerIdx, uint32 entityIdx, Proto::EntityReplicateNtf &ntf);
        void PackUpdate(uint32 observerIdx, uint32 entityIdx, Proto::EntityReplicateNtf &ntf);
        void PackDespawn(uint32 observerIdx, uint32 entityIdx, Proto::EntityReplicateNtf &ntf);

        entt::registry &_reg;
        SendFn          _sendFn;
        std::unordered_map<uint32, std::unordered_set<uint32>> _aoiState; // observer → 可见
    };

} // namespace MMO
```

```cpp
// Src/World/System/SystemReplicate.cpp
#include "World/System/SystemReplicate.h"

#include "Common/Log/Log.h"
#include "World/Component/Health.h"
#include "World/Component/PlayerConn.h"
#include "World/Component/Position.h"
#include "World/Component/Tags.h"

#include <Replicate.pb.h>
#include <MsgID.pb.h>

namespace MMO
{

    void ReplicateSystem::Update(float /*dt*/,
                                 const std::unordered_map<uint32, std::unordered_set<uint32>> &/*prevState*/,
                                 const std::vector<std::pair<uint32, uint32>> &enters,
                                 const std::vector<std::pair<uint32, uint32>> &leaves)
    {
        // ── 1. 处理 leave → Despawn ──
        for (auto &[obsIdx, entIdx] : leaves)
        {
            auto it = _aoiState.find(obsIdx);
            if (it != _aoiState.end())
            {
                it->second.erase(entIdx);
            }
            // 玩家离开视野 → 发给该玩家的 Despawn
            if (auto *conn = _reg.try_get<PlayerConn>(entt::entity(static_cast<entt::id_type>(obsIdx))))
            {
                Proto::EntityReplicateNtf ntf;
                PackDespawn(obsIdx, entIdx, ntf);
                SendNtf(conn->sessionID, ntf);
            }
        }

        // ── 2. 处理 enter → Spawn ──
        for (auto &[obsIdx, entIdx] : enters)
        {
            _aoiState[obsIdx].insert(entIdx);
            if (auto *conn = _reg.try_get<PlayerConn>(entt::entity(static_cast<entt::id_type>(obsIdx))))
            {
                Proto::EntityReplicateNtf ntf;
                PackSpawn(obsIdx, entIdx, ntf);
                SendNtf(conn->sessionID, ntf);
            }
        }

        // ── 3. 活跃实体 → Update（每帧全量位置 + 脏血量）──
        // 简化：遍历 _aoiState 所有观察者，对其可见实体打包 Update
        for (auto &[obsIdx, visible] : _aoiState)
        {
            auto *conn = _reg.try_get<PlayerConn>(entt::entity(static_cast<entt::id_type>(obsIdx)));
            if (!conn)
            {
                continue;
            }

            Proto::EntityReplicateNtf ntf;
            for (uint32 entIdx : visible)
            {
                PackUpdate(obsIdx, entIdx, ntf);
            }
            if (ntf.updates_size() > 0)
            {
                SendNtf(conn->sessionID, ntf);
            }
        }
    }

    void ReplicateSystem::PackSpawn(uint32 /*observerIdx*/, uint32 entityIdx, Proto::EntityReplicateNtf &ntf)
    {
        auto e = entt::entity(static_cast<entt::id_type>(entityIdx));
        auto *spawn = ntf.add_spawns();
        spawn->set_entity_id64(static_cast<uint64>(entityIdx)); // MVP: 低 32 位即 EnTT index（同 EntityRegistry index）

        if (auto *pos = _reg.try_get<Position>(e))
        {
            auto *pd = spawn->mutable_position();
            pd->set_x(static_cast<int32>(pos->x * 100.0f));
            pd->set_y(static_cast<int32>(pos->y * 100.0f));
            pd->set_z(static_cast<int32>(pos->z * 100.0f));
        }
        if (auto *hp = _reg.try_get<Health>(e))
        {
            spawn->set_hp_current(hp->current);
            spawn->set_hp_max(hp->max);
        }
        if (_reg.all_of<MonsterTag>(e))
        {
            spawn->set_entity_type(static_cast<uint32>(EEntityType::ENTITY_MONSTER));
        }
        else if (_reg.all_of<PlayerTag>(e))
        {
            spawn->set_entity_type(static_cast<uint32>(EEntityType::ENTITY_PLAYER));
        }
    }

    void ReplicateSystem::PackUpdate(uint32 /*observerIdx*/, uint32 entityIdx, Proto::EntityReplicateNtf &ntf)
    {
        auto e = entt::entity(static_cast<entt::id_type>(entityIdx));
        auto *update = ntf.add_updates();
        update->set_entity_id64(static_cast<uint64>(entityIdx));

        if (auto *pos = _reg.try_get<Position>(e))
        {
            auto *pd = update->mutable_position();
            pd->set_x(static_cast<int32>(pos->x * 100.0f));
            pd->set_y(static_cast<int32>(pos->y * 100.0f));
            pd->set_z(static_cast<int32>(pos->z * 100.0f));
        }
        if (auto *hp = _reg.try_get<Health>(e))
        {
            update->set_hp_current(hp->current);
        }
        if (_reg.all_of<DeadTag>(e))
        {
            update->set_is_dead(true);
        }
        if (_reg.all_of<CombatTag>(e))
        {
            update->set_is_in_combat(true);
        }
    }

    void ReplicateSystem::PackDespawn(uint32 /*observerIdx*/, uint32 entityIdx, Proto::EntityReplicateNtf &ntf)
    {
        auto *despawn = ntf.add_despawns();
        despawn->set_entity_id64(static_cast<uint64>(entityIdx));
    }

    void ReplicateSystem::SendNtf(uint32 sessionID, const Proto::EntityReplicateNtf &ntf)
    {
        size_t bodySize = static_cast<size_t>(ntf.ByteSizeLong());
        if (bodySize == 0)
        {
            return;
        }
        auto buf = std::vector<uint8>(bodySize);
        ntf.SerializeToArray(buf.data(), static_cast<int>(bodySize));
        _sendFn(sessionID, buf.data(), bodySize);
    }

} // namespace MMO
```

> **注意**：`SendNtf` 的 `_sendFn` 由 WorldServer 提供——内部做加密 + PacketHeader 包装 +
> `SendToGate`（复用既有 `SendToClient` 模板逻辑）。

---

## 4. 带宽预算（生产版）

MVP 的 `PackUpdate` 每帧对可见实体全量打包——5w 实体场景会超带宽。生产版加预算：

```cpp
// 在 Update 内，每玩家打包前初始化预算
static constexpr size_t kReplicateBudgetBytes = 64 * 1024; // 64KB/玩家/帧

Proto::EntityReplicateNtf ntf;
size_t used = 0;

for (uint32 entIdx : visible)
{
    // 先估算单实体 Update 大小（position 12B + hp 2B + tags ~2B ≈ 20B）
    constexpr size_t kEstUpdateBytes = 24;
    if (used + kEstUpdateBytes > kReplicateBudgetBytes)
    {
        break; // 超预算截断（优先级由可见集排序保证：先近后远）
    }
    PackUpdate(obsIdx, entIdx, ntf);
    used += kEstUpdateBytes;
}
```

**优先级排序**（`kReplicatePrioritySteps = 3`）：可见集按距离分 3 档（近/中/远），
打包按"近档优先"顺序——超预算时远档被截断。

---

## 5. 并行打包（决策 1：方案 C——串行模拟 + 并行复制）

模拟线程（LogicThread）完成后，**复制打包是只读操作**（读 Position/Health/Tags + 序列化），
可并行。**决策 1 定稿**：模拟串行，复制打包并行。

### 5.1 打包纯函数（核心设计——无副作用，可并行）

```cpp
// Src/World/System/SystemReplicate.h（重构）
#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>

#include "Common/Core/Types.h"
#include "Common/ECS/Scene.h"

namespace MMO
{

    /**
     * @brief 单个玩家的复制打包（纯函数，线程安全）
     *
     * 只读 registry——模拟线程写完、打包线程只读。
     * 返回序列化字节（每玩家独立 buffer，无共享写）。
     *
     * @param scene      场景（Scene::ToEntityID 用）
     * @param observerIdx 观察者 EnTT index
     * @param visible    该玩家可见实体集
     * @param enters     本帧 enter（(obsIdx, entIdx)）
     * @param leaves     本帧 leave
     * @return 序列化后的 EntityReplicateNtf 字节（空 = 无更新）
     */
    std::vector<uint8> PackPlayerReplicate(ECS::Scene             &scene,
                                           uint32                  observerIdx,
                                           const std::unordered_set<uint32> &visible,
                                           const std::vector<std::pair<uint32, uint32>> &enters,
                                           const std::vector<std::pair<uint32, uint32>> &leaves);

    /**
     * @brief 复制调度器——串行入口，并行打包，统一发送
     *
     * 遍历玩家 → 投递 PackPlayerReplicate 到线程池 → join → 逐玩家发送。
     */
    class ReplicateScheduler
    {
    public:
        using SendFn = std::function<void(uint32 sessionID, const uint8 *data, size_t len)>;

        ReplicateScheduler(ECS::Scene &scene, SendFn sendFn, size_t workerCount = 4)
            : _scene(scene), _sendFn(std::move(sendFn)), _workerCount(workerCount)
        {
        }

        /**
         * @brief 每帧调用（模拟完成后）
         * @param aoiState 当前玩家可见集（AOI 输出）
         */
        void Update(const std::unordered_map<uint32, std::unordered_set<uint32>> &aoiState,
                    const std::vector<std::pair<uint32, uint32>> &enters,
                    const std::vector<std::pair<uint32, uint32>> &leaves);

    private:
        ECS::Scene   &_scene;
        SendFn        _sendFn;
        size_t        _workerCount;
    };

} // namespace MMO
```

### 5.2 调度器实现（串行入口 + 并行打包）

```cpp
// Src/World/System/SystemReplicate.cpp
#include "World/System/SystemReplicate.h"

#include <atomic>
#include <thread>
#include <vector>

#include "Common/Log/Log.h"
#include "World/Component/PlayerConn.h"

namespace MMO
{

    void ReplicateScheduler::Update(
        const std::unordered_map<uint32, std::unordered_set<uint32>> &aoiState,
        const std::vector<std::pair<uint32, uint32>> &enters,
        const std::vector<std::pair<uint32, uint32>> &leaves)
    {
        // ── 并行打包：每玩家一个任务 ──
        struct TaskResult
        {
            uint32             sessionID;
            std::vector<uint8> bytes;
        };
        std::vector<TaskResult> results;
        results.reserve(aoiState.size());

        std::atomic<size_t> nextTask {0};
        std::vector<uint32> observers;
        observers.reserve(aoiState.size());
        for (auto &[obsIdx, _] : aoiState)
        {
            observers.push_back(obsIdx);
        }

        const size_t taskCount = observers.size();
        std::vector<std::thread> workers;
        const size_t wc = std::min(_workerCount, taskCount);
        for (size_t w = 0; w < wc; ++w)
        {
            workers.emplace_back([&]() {
                while (true)
                {
                    const size_t i = nextTask.fetch_add(1, std::memory_order_relaxed);
                    if (i >= taskCount)
                    {
                        break;
                    }
                    const uint32 obsIdx = observers[i];
                    auto it = aoiState.find(obsIdx);
                    if (it == aoiState.end())
                    {
                        continue;
                    }
                    auto bytes = PackPlayerReplicate(_scene, obsIdx, it->second, enters, leaves);
                    if (bytes.empty())
                    {
                        continue;
                    }
                    // 找 sessionID
                    auto e = entt::entity(static_cast<entt::id_type>(obsIdx));
                    if (auto *conn = _scene.Registry().try_get<PlayerConn>(e))
                    {
                        results.emplace_back(TaskResult{conn->sessionID, std::move(bytes)});
                    }
                }
            });
        }
        for (auto &t : workers)
        {
            t.join();
        }

        // ── 串行发送（发送线程安全由 IO 层保证）──
        for (auto &r : results)
        {
            _sendFn(r.sessionID, r.bytes.data(), r.bytes.size());
        }
    }

    std::vector<uint8> PackPlayerReplicate(
        ECS::Scene &scene,
        uint32 observerIdx,
        const std::unordered_set<uint32> &visible,
        const std::vector<std::pair<uint32, uint32>> &enters,
        const std::vector<std::pair<uint32, uint32>> &leaves)
    {
        // 实现同 §3 的 PackSpawn/PackUpdate/PackDespawn，但：
        // 1. 用 scene.ToEntityID(e) 拿完整 uint64 EntityID
        // 2. enter/leave 只处理 observerIdx 相关的项
        // 3. 输出序列化字节（空 ntf → 返回空）
        Proto::EntityReplicateNtf ntf;

        for (auto &[obs, ent] : leaves)
        {
            if (obs == observerIdx)
            {
                ntf.add_despawns()->set_entity_id(scene.ToEntityID(entt::entity(static_cast<entt::id_type>(ent))));
            }
        }
        for (auto &[obs, ent] : enters)
        {
            if (obs == observerIdx)
            {
                // PackSpawn（用 ToEntityID）
                ...
            }
        }
        for (uint32 entIdx : visible)
        {
            // PackUpdate（用 ToEntityID）
            ...
        }

        if (ntf.spawns_size() == 0 && ntf.updates_size() == 0 && ntf.despawns_size() == 0)
        {
            return {};
        }
        const size_t sz = static_cast<size_t>(ntf.ByteSizeLong());
        std::vector<uint8> bytes(sz);
        ntf.SerializeToArray(bytes.data(), static_cast<int>(sz));
        return bytes;
    }

} // namespace MMO
```

> **关键保证**：
> 1. `PackPlayerReplicate` 是**纯函数**——入参 `scene`/`visible`/`enters`/`leaves` 都是
>    只读引用，输出独立字节 buffer——**无共享写、无锁**。
> 2. `results` 的 `emplace_back` 在**多 worker 下是竞态**！——修正：每个 worker 收集
>    自己的结果再合并，或 `results` 用 `std::mutex` 保护，或按 `observers[i]` 预分配槽位
>    （**推荐**：`std::vector<TaskResult> results(taskCount)`，`results[i]` 由第 i 个任务独占写）。
> 3. **只读 barrier**：`Update` 必须在模拟完成后调用（World::Tick 的 Replicate 阶段），
>    且**下一帧模拟开始前 join 完成**——`Update` 同步 join（上面实现），保证。
> 4. 演进路径：先同步 join 正确 → 再改成"投递后不阻塞、下一帧前 join"（流水线化）。

### 5.3 演进路径（先串行 → 后并行）

```cpp
// 阶段 1（MVP）：同步——Update 内部串行打包（_workerCount = 1）
ReplicateScheduler(..., 1);  // 单 worker = 串行，行为正确

// 阶段 2（并行）：Update 内部多 worker（_workerCount = 4）
// 阶段 3（流水线）：模拟与复制重叠——模拟线程开始下一帧，复制线程继续上一帧打包
//   需保证：registry 上帧只读快照（复制用）与本帧写（模拟用）隔离——用双 buffer 或
//   复制读"上帧结束时的注册表状态"。此阶段才需要 registry 快照语义。
```

---

## 6. WorldServer 接线

```cpp
// WorldServer.h 追加
#include "World/System/SystemReplicate.h"

// 私有成员
std::unique_ptr<ReplicateSystem> _replicateSystem;
std::unordered_map<uint32, std::unordered_set<uint32>> _aoiState; // observer → 可见
std::vector<std::pair<uint32, uint32>> _aoiEnters;
std::vector<std::pair<uint32, uint32>> _aoiLeaves;
```

```cpp
// WorldServer.cpp

// Init 中（InitWorlds 之后）：
_replicateSystem = std::make_unique<ReplicateSystem>(
    _worlds[0]->Scene().Registry(),
    [this](uint32 sessionID, const uint8 *data, size_t len) {
        // 加密 + 包装 + 发送（复用 SendToClient 逻辑，但 msgID 固定 MSG_ENTITY_REPLICATE_NTF）
        SendEncrypted(sessionID, Proto::MSG_ENTITY_REPLICATE_NTF, data, len);
    });

// OnTick 中（world->Tick(dt) 之后）：
void WorldServer::OnTick(float dt)
{
    ProcessUnroutedMessages();
    ProcessControlMessages();

    for (auto &world : _worlds)
    {
        world->Tick(dt); // 内部跑 Movement/SpatialIndex/AOI，产出 enter/leave
    }

    // AOI 事件 → 复制
    auto &scene = _worlds[0]->Scene();
    auto &grid  = scene.Grid(); // 需要 World 暴露 Grid（或 Scene 持有）
    // SystemAOI 在 World::Tick 的 AOI 阶段跑，事件存到 WorldServer 侧
    // 简化：Replicate 阶段消费 World 侧累积的 enter/leave
    if (_replicateSystem)
    {
        _replicateSystem->Update(dt, _aoiState, _aoiEnters, _aoiLeaves);
    }

    // 过载保护（既有逻辑）
    ...
}
```

> **注意**：AOI 事件在 `World::Tick` 内产生（SystemAOI 输出到 World 持有的 buffer），
> `ReplicateSystem` 消费后清空。本篇用 `WorldServer` 侧 buffer 桥接，后续可收紧到
> `World` 内部（复制系统归 World 管）。

---

## 7. 构建脚本变动

- `Src/Proto/Replicate.proto`：`entity_id` **直接改 uint64**（决策 2）→ 重新生成 `Replicate.pb.h/.cc`
- `Src/Proto/Login.proto`：`player_id` 改 uint64（连带）
- `Src/World/xmake.lua`：`add_files("**.cpp")` 已覆盖 `System/SystemReplicate.cpp`——无需改
- proto 生成由既有 `proto_gen` rule 自动处理（改 `.proto` 后 xmake 检测 mtime 重新生成）

---

## 8. 验证步骤（本篇验收）

```powershell
# 1. 构建（proto 变更会自动重新生成）
xmake build WorldServer

# 2. 端到端（TestClient）：
#    - 登录 → EnterWorld → 玩家 A 进入世界
#    - 服务器 spawn 一个怪物 B 在 A 视野内
#    - 预期 A 收到 EntityReplicateNtf: spawn(B)
#    - B 移动 → A 收到 update(B, position)
#    - B 移出视野 → A 收到 despawn(B)
```

**验收标准**：
- [ ] 构建零错误
- [ ] `Replicate.pb.h` 重新生成 `entity_id` 为 uint64
- [ ] `LoginEnterWorldRsp.player_id` 为 uint64
- [ ] spawn/update/despawn 事件流正确（用 `Scene::ToEntityID` 拿完整 ID）
- [ ] 序列化 + 加密 + 发送链路通（TestClient 能收到）
- [ ] 带宽预算截断不崩溃

---

## 9. 踩坑预警

1. **完整 EntityID**：`entity_id` 是 uint64——打包**必须**用 `Scene::ToEntityID(e)`
   （含 scene/version 位），不能只传 EnTT `to_integral` 的 uint32。
2. **`try_get` vs `get`**：打包时实体可能被并发销毁（跨帧）——用 `try_get` 安全判空，
   `get` 会 assert。
3. **并行打包只读**（决策 1）：打包线程并发读 registry，**模拟线程必须已写完**——
   用"模拟结束 → 快照 → 并行打包 → join"barrier 保证；打包函数必须是纯只读。
4. **`PackPlayerNtf` 纯函数化**（决策 1）：打包逻辑不持有 `_sendFn`，由调度器统一发送——
   这样并行打包任务间零共享、零锁。
4. **复制与模拟同线程**：MVP 串行，复制在模拟后立即执行（同 LogicThread）——registry
   无并发风险。并行是后续优化。
5. **玩家自己**：玩家实体在 `_aoiState` 里被自己可见（AOI 排除了 observer 自身）——
   复制时玩家自己单独发 spawn（登录时）或从 AOI 特殊处理。
