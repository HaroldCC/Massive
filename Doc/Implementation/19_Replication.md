# 脚本引擎 #19：网络复制与 AOI——空间索引 + 差量同步

> 状态：**设计中**（2026-07-15）
> 关联：[09_ScriptEngine](09_ScriptEngine.md)（总纲 §8.2 Phase 7）、[12_CPPSystems](12_CPPSystems.md)（CPPSystems 调度）、[network_replication](../前期设计/ecs/network_replication.html)（上游设计）
> 前置依赖：Position 组件已定义、DirtyTracker 可用、EnTT entity 生命周期稳定
> 对应 Phase：Phase 4

## 1. 定位

网络复制负责将服务端 EnTT 组件变更同步到客户端。核心机制：**AOI（Area of Interest）**管理每个客户端"能看到哪些实体"→**差量序列化**只发送变化的组件。这是 C++ 系统——走 EnTT 热路径，不经过 DECS。

## 2. 架构与数据流

```
EnTT Component 变更（Position / Health / CombatTag）
        │
DirtyTracker.Mark<T>(entity)  ← 在 MovementSystem/RecalcStatsSystem 中标记
        │
每 Tick: ReplicateSystem (C++)
  ├── 1. AOISystem: 空间索引更新 → 计算每个 entity 的 visible set
  ├── 2. 对每个 client/entity 的 visible set:
  │     遍历 DirtyTracker
  │       ├── 新进入 AOI → 同步完整 entity state (SpawnNtf)
  │       ├── 脏组件     → 只同步变更组件 (UpdateNtf)
  │       ├── 离开 AOI   → 通知客户端移除 (DespawnNtf)
  │       └── 无变更     → 跳过
  └── 3. 序列化 → 加密 → SendToClient
```

## 3. AOI——十字链表 vs 网格

### 决策：MVP 使用全量遍历，Phase 4+ 实现网格分区

| 方案 | 复杂度 | 内存 | Phase 4 适用？ |
|------|--------|------|---------------|
| 全量遍历 | O(N×M) N=entity, M=player | 0 | ✅ MVP——N≤100, M≤10 完全够用 |
| 网格分区 | O(N/M) | grid 开销 ~50KB | Phase 4+——MMO 生产量级 |
| 十字链表 | O(1) 插入/O(visible) 查询 | 节点开销 ~40B/entity | 远期——优化选择 |

Phase 4 的目标是验证整套链路（AOI → Dirty → 序列化 → 发送），性能不必最优。全量遍历在 100 entity × 10 player 量级下 < 0.01ms。

### 3.1 Phase 4 简单实现

```cpp
// Src/World/System/AOISystem.cpp
namespace MMO
{

/**
 * @brief 计算每个 player 的 visible entities
 *
 * Phase 4 简化：遍历所有 entity 的 Position → O(N²)
 * 生产期替换为空间索引（网格 / R-tree / 十字链表）
 */
struct VisibleSet
{
    std::vector<uint32_t> entityIDs;
    float viewRadiusXZ = 100.0f;  // 水平可见范围（米）
    float viewRadiusY  = 15.0f;   // 垂直可见范围
};

void SystemAOI(ECS::Scene &scene,
               std::unordered_map<uint32_t, VisibleSet> &outVisibleSets)
{
    auto &reg = scene.Registry();

    // 采集所有有 Position 的 entity
    struct EntityEntry { uint32_t id; float x, y, z; };
    std::vector<EntityEntry> entities;
    {
        auto view = reg.view<Position>();
        entities.reserve(view.size_hint());
        for (auto [e, pos] : view.each())
        {
            entities.push_back({
                static_cast<uint32_t>(entt::to_integral(e)),
                pos.x, pos.y, pos.z
            });
        }
    }

    // 对每个 player（有 Position + PlayerTag 的 entity）计算 AOI
    {
        auto players = reg.view<Position, PlayerTag>();
        for (auto [e, pos] : players.each())
        {
            uint32_t pid = static_cast<uint32_t>(entt::to_integral(e));
            VisibleSet &vs = outVisibleSets[pid];
            vs.viewRadiusXZ = 100.0f;
            vs.viewRadiusY  = 15.0f;
            vs.entityIDs.reserve(entities.size());

            for (auto &entry : entities)
            {
                if (entry.id == pid) continue;  // 不包含自己

                float dx = entry.x - pos.x;
                float dz = entry.z - pos.z;
                float dy = entry.y - pos.y;

                // 水平范围 + 垂直范围
                if (dx * dx + dz * dz <= vs.viewRadiusXZ * vs.viewRadiusXZ
                    && std::abs(dy) <= vs.viewRadiusY)
                {
                    vs.entityIDs.push_back(entry.id);
                }
            }
        }
    }
}

} // namespace MMO
```

## 4. 网络复制——差量序列化

### 4.1 复制协议

```protobuf
// Src/Proto/Replicate.proto（新建）
syntax = "proto3";
package MMO.Proto;

// Entity 进入 AOI——完整状态同步
message EntitySpawnNtf {
    uint32 entity_id = 1;
    uint32 entity_type = 2;
    PositionDelta position = 3;
    int64  level = 4;
    int64  hp_current = 5;
    int64  hp_max = 6;
    int64  mp_current = 7;
    int64  mp_max = 8;
    uint32 move_state = 9;
    uint64 owner_player_id = 10;   // 所属玩家（宠物召唤物等）
}

// Entity 状态更新——仅变更的组件
message EntityUpdateNtf {
    uint32 entity_id = 1;
    // 使用 optional——只有变更的字段才序列化
    optional PositionDelta position = 2;
    optional int32 hp_current = 3;
    optional int32 hp_max = 4;
    optional int32 mp_current = 5;
    optional bool  is_dead = 6;
    optional bool  is_in_combat = 7;
}

// Entity 离开 AOI
message EntityDespawnNtf {
    uint32 entity_id = 1;
}

// 批量通知——一帧内的所有 AOI 变更合并到一个包
message EntityReplicateNtf {
    repeated EntitySpawnNtf   spawns   = 1;
    repeated EntityUpdateNtf  updates  = 2;
    repeated EntityDespawnNtf despawns = 3;
}

// 整数坐标——节省带宽（float32 → varint）
message PositionDelta {
    int32 x = 1;  // 实际坐标 = displayed / 100
    int32 y = 2;
    int32 z = 3;
}
```

### 4.2 ReplicateSystem 实现

```cpp
// Src/World/System/ReplicateSystem.cpp
namespace MMO
{

struct AOIState {
    std::unordered_set<uint32_t> visibleEntities;  // 当前可见的 entity
};

void SystemReplicate(ECS::Scene &scene,
                     WorldServer &worldServer,
                     DirtyTracker &dirtyTracker,
                     const std::unordered_map<uint32_t, VisibleSet> &visibleSets,
                     std::unordered_map<uint32_t, AOIState> &aoiStates)
{
    auto &reg = scene.Registry();
    auto &sessions = worldServer.Sessions();  // session → entity 映射

    for (auto &[sessionID, ws] : sessions)
    {
        if (ws.disconnected) continue;

        uint32_t playerEID = ws.entity.entityId;
        uint32_t sceneID   = ws.entity.sceneId;

        auto itVs = visibleSets.find(playerEID);
        const auto &vs = (itVs != visibleSets.end()) ? itVs->second : VisibleSet{};

        auto &aoiState = aoiStates[playerEID];

        Proto::EntityReplicateNtf ntf;

        // 1. 新进入 AOI → SpawnNtf
        for (uint32_t eid : vs.entityIDs)
        {
            if (aoiState.visibleEntities.contains(eid)) continue;

            // 新 entity — 完整同步
            auto *spawn = ntf.add_spawns();
            spawn->set_entity_id(eid);

            // 序列化 EnTT 组件到 protobuf
            if (reg.all_of<Position>(entt::entity(eid)))
            {
                auto &pos = reg.get<Position>(entt::entity(eid));
                auto *pd = spawn->mutable_position();
                pd->set_x(static_cast<int32_t>(pos.x * 100.0f));
                pd->set_y(static_cast<int32_t>(pos.y * 100.0f));
                pd->set_z(static_cast<int32_t>(pos.z * 100.0f));
            }

            if (reg.all_of<Health>(entt::entity(eid)))
            {
                auto &hp = reg.get<Health>(entt::entity(eid));
                spawn->set_hp_current(hp.current);
                spawn->set_hp_max(hp.max);
            }

            if (reg.all_of<EntityTypeTag>(entt::entity(eid)))
            {
                auto &tag = reg.get<EntityTypeTag>(entt::entity(eid));
                spawn->set_entity_type(static_cast<uint32_t>(tag.type));
            }

            aoiState.visibleEntities.insert(eid);
        }

        // 2. 脏组件 → UpdateNtf
        for (uint32_t eid : vs.entityIDs)
        {
            if (!aoiState.visibleEntities.contains(eid)) continue;

            auto *update = ntf.add_updates();
            update->set_entity_id(eid);

            bool hasDirty = false;

            if (dirtyTracker.IsDirty<Position>(eid))
            {
                auto &pos = reg.get<Position>(entt::entity(eid));
                auto *pd = update->mutable_position();
                pd->set_x(static_cast<int32_t>(pos.x * 100.0f));
                pd->set_y(static_cast<int32_t>(pos.y * 100.0f));
                pd->set_z(static_cast<int32_t>(pos.z * 100.0f));
                hasDirty = true;
            }

            if (dirtyTracker.IsDirty<Health>(eid))
            {
                auto &hp = reg.get<Health>(entt::entity(eid));
                update->set_hp_current(hp.current);
                update->set_hp_max(hp.max);
                hasDirty = true;
            }

            if (dirtyTracker.IsDirty<CombatTag>(eid))
            {
                update->set_is_in_combat(reg.all_of<CombatTag>(entt::entity(eid)));
                hasDirty = true;
            }

            if (dirtyTracker.IsDirty<DeadTag>(eid))
            {
                update->set_is_dead(reg.all_of<DeadTag>(entt::entity(eid)));
                hasDirty = true;
            }

            if (!hasDirty)
            {
                // 无变更——移除这个 update 条目
                ntf.mutable_updates()->RemoveLast();
            }
        }

        // 3. 离开 AOI → DespawnNtf
        for (auto it = aoiState.visibleEntities.begin();
             it != aoiState.visibleEntities.end(); )
        {
            uint32_t eid = *it;
            bool stillVisible = false;
            for (auto &vid : vs.entityIDs)
            {
                if (vid == eid) { stillVisible = true; break; }
            }

            if (!stillVisible)
            {
                auto *despawn = ntf.add_despawns();
                despawn->set_entity_id(eid);
                it = aoiState.visibleEntities.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // 4. 序列化 + 发送——只有当有数据时才发送
        if (ntf.spawns_size() > 0 || ntf.updates_size() > 0 || ntf.despawns_size() > 0)
        {
            // 零分配序列化
            size_t bodySize = static_cast<size_t>(ntf.ByteSizeLong());
            auto buf = ByteBuffer::Own(bodySize);
            ntf.SerializeToArray(buf.WritePtr(), static_cast<int>(bodySize));
            buf.SetWritePos(bodySize);

            worldServer.SendRawToClient(sessionID,
                Proto::MSG_ENTITY_REPLICATE_NTF, buf.Data(), buf.Size());
        }
    }
}

} // namespace MMO
```

### 4.3 脏标记清理

```cpp
// 每 Tick 发送完成后——清理所有脏标记
void DirtyTracker::ClearAll()
{
    for (auto &[componentType, eids] : _dirtyEntities)
    {
        eids.clear();
    }
}
```

## 5. 集成到 RunCPPSystems

```cpp
void WorldServer::RunCPPSystems(ECS::Scene &scene, float dt, float tickTime)
{
    // 1. 移动
    SystemMovement(scene, dt);

    // 2. 属性重算
    SystemRecalcStats(scene, *_scriptBridge);

    // 3. 战斗超时
    SystemCombatTimeout(scene, tickTime, dt);

    // 4. AOI（Phase 4 新增）
    std::unordered_map<uint32_t, VisibleSet> visibleSets;
    SystemAOI(scene, visibleSets);

    // 5. 网络复制（Phase 4 新增）
    SystemReplicate(scene, *this, _dirtyTracker, visibleSets, _aoiStates);

    // 6. 清理脏标记——发送完毕后
    _dirtyTracker.ClearAll();
}
```

## 6. DirtyTracker 扩展

已有的 `DirtyTracker.h` 需要扩展为按 entity 跟踪：

```cpp
// Src/Common/ECS/DirtyTracker.h — 修订
class DirtyTracker
{
public:
    template <typename T>
    void Mark(uint32_t entityID) {
        _dirtyEntities[ComponentTypeID<T>()].insert(entityID);
    }

    template <typename T>
    bool IsDirty(uint32_t entityID) const {
        auto it = _dirtyEntities.find(ComponentTypeID<T>());
        if (it == _dirtyEntities.end()) return false;
        return it->second.contains(entityID);
    }

    void ClearAll();

private:
    // componentTypeID → {entityID, entityID, ...}
    std::unordered_map<size_t, std::unordered_set<uint32_t>> _dirtyEntities;

    template <typename T>
    static size_t ComponentTypeID() {
        static const size_t id = _nextID++;
        return id;
    }
    static inline size_t _nextID = 0;
};
```

## 7. 文件清单

```
Src/World/System/
├── AOISystem.cpp                 # 新建——AOI 空间查询 + visible set 计算
├── ReplicateSystem.cpp           # 新建——差量序列化 + 发送
├── MovementSystem.cpp            # 已修改——每帧标记 Position dirty
├── RecalcStatsSystem.cpp         # 已修改——每帧标记 Health dirty
└── CombatTimeoutSystem.cpp       # 已修改——标记 CombatTag dirty

Src/Common/ECS/
├── DirtyTracker.h                # 修改——扩展为 per-entity dirty 跟踪

Src/Proto/
├── Replicate.proto               # 新建——EntitySpawnNtf/UpdateNtf/DespawnNtf

Src/Proto/
└── MsgID.proto                   # 追加 MSG_ENTITY_REPLICATE_NTF
```

## 8. Phase 4 扩展点

| 功能 | Phase 4 状态 | 扩展内容 |
|------|-------------|---------|
| 空间索引 | O(N²) 全量遍历 | 网格分区——O(N/M) 查询性能 |
| 整数坐标压缩 | Position × 100 → int32 | Delta 压缩——节省更多带宽 |
| 优先级队列 | 全量发送 | 按距离优先——近的 entity 先发 |
| 带宽限制 | 无 | 每帧最大发送字节数限制 |
| 移动预测 | 未实现 | 客户端插值 + 服务器修正 |
| 脚本组件复制 | 未实现 | DECS 组件 → C++ 序列化 → 客户端 |
| Interest 管理 | 全量 AOI | 按 entity 类型过滤——客户端不需要看到所有 entity |
