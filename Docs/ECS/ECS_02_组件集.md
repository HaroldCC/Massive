# 组件集（ECS_02）

> 契约见 `ECS_00` §4。本篇给出**可照抄实现**：
> 组件声明（C++ 固定类型）→ `DirtyIndex`（脏标记）→ 组件所有权模型。
> 交付后：组件集 + 脏追踪可用，`SystemMovement` 能在 ECS_03 中直接消费。

---

## 1. 设计原则

1. **组件是 C++ 固定的 POD struct**，EnTT SoA 存储（`entt::registry` 的 `basic_storage`）。
2. **组件所有权**：`Scene`（entt::registry）唯一所有者，`Scene::EmplaceComponent` 是唯一写入入口。
3. **脚本只读快照**：脚本通过 `Bridge_*` 拿**值拷贝**（`float3`/`int32` 等 POD），
   不持引用——热重载安全（铁律 1）。
4. **DirtyIndex 驱动复制**：系统写组件后显式 `MarkDirty`，复制系统按脏集打包（ECS_05）。

---

## 2. 组件清单（World 专用，`Src/World/Component/`）

```cpp
// Src/World/Component/Position.h
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 3D 世界坐标（C++ 高频组件，EnTT SoA 存储）
     *
     * 复制用整数坐标（float32 × 100 → int32），见 Replicate.proto PositionDelta。
     */
    struct Position
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

} // namespace MMO
```

```cpp
// Src/World/Component/Velocity.h
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 3D 移动速度（units/s）
     *
     * Movement 系统写入，Movement 阶段消费。脚本只读。
     */
    struct Velocity
    {
        float vx = 0.0f;
        float vy = 0.0f;
        float vz = 0.0f;
    };

} // namespace MMO
```

```cpp
// Src/World/Component/Health.h
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 生命值
     *
     * 战斗系统写入（含脚本经 Bridge 写入），复制系统读。
     */
    struct Health
    {
        int32 current = 0;
        int32 max     = 0;
    };

} // namespace MMO
```

```cpp
// Src/World/Component/Tags.h
#pragma once

namespace MMO
{

    /** @brief 实体已死亡（currentHp == 0） */
    struct DeadTag {};

    /** @brief 实体处于战斗状态 */
    struct CombatTag {};

    /** @brief 实体被眩晕/定身 */
    struct StunnedTag {};

    /** @brief 实体为玩家 */
    struct PlayerTag {};

    /** @brief 实体为怪物/NPC */
    struct MonsterTag {};

    /** @brief 实体休眠（AOI 外，跳过模拟） */
    struct DormantTag {};

} // namespace MMO
```

```cpp
// Src/World/Component/EntityType.h
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 实体类型——EnTT + Protobuf 共用（EntitySpawnNtf.entity_type）
     *
     * 新增类型只追加到末尾，已有值永不改变。
     */
    enum class EEntityType : int32
    {
        ENTITY_PLAYER  = 0,
        ENTITY_NPC     = 1,
        ENTITY_MONSTER = 2,
    };

} // namespace MMO
```

```cpp
// Src/World/Component/BattleStats.h
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 战斗属性（脚本经 Bridge 读写的完整属性集）
     *
     * 值语义：脚本拿快照，改完经 Bridge 写回（写回触发 MarkDirty）。
     * 后续可拆分为 base + buff 两层，MVP 先单层。
     */
    struct BattleStats
    {
        int32 attack       = 0; // 物理攻击
        int32 defense      = 0; // 物理防御
        int32 magicAttack  = 0; // 魔法攻击
        int32 magicDefense = 0; // 魔法防御
        int32 critRate     = 0; // 暴击率（万分比）
        int32 critDamage   = 0; // 暴击伤害倍率（万分比）
        int32 dodgeRate    = 0; // 闪避率（万分比）
        int32 hitRate      = 0; // 命中率（万分比）
        int32 attackSpeed  = 0; // 攻击速度（万分比）
        int32 moveSpeed    = 0; // 移动速度（万分比）
        int32 maxHp        = 0; // 最大生命值
        int32 maxMp        = 0; // 最大魔法值
    };

} // namespace MMO
```

```cpp
// Src/World/Component/PlayerConn.h
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 玩家连接归属——实体 ↔ sessionID 双向映射的一侧
     *
     * 挂在玩家实体上，复制系统/离线处理通过它找 session。
     * 另一侧是 WorldSession::entityID（ECS_01 已加）。
     */
    struct PlayerConn
    {
        uint32 sessionID = 0;
        uint16 gateServerID = 0;
    };

} // namespace MMO
```

```cpp
// Src/World/Component/AIState.h
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 怪物 AI 状态（低频决策，脚本 [game_system] 读写）
     *
     * 状态机：IDLE → PATROL → COMBAT → FLEE
     * 脚本读取/写入均经 Bridge（写回 MarkDirty）。
     */
    enum class EAIState : int32
    {
        IDLE   = 0,
        PATROL = 1,
        COMBAT = 2,
        FLEE   = 3,
    };

    struct AIState
    {
        EAIState state     = EAIState::IDLE;
        uint64   targetID  = 0; // 当前目标实体
        float    stateTime = 0.0f; // 当前状态持续时长
    };

} // namespace MMO
```

---

## 3. `DirtyIndex` — 复制脏标记

```cpp
// Src/Common/ECS/DirtyIndex.h
#pragma once

#include <cstdint>
#include <vector>

#include "Common/Core/Types.h"

namespace MMO::ECS
{

    /**
     * @brief 按组件类型分桶的脏实体标记
     *
     * 设计：每组件类型一个独立桶（编译期模板参数），桶内是 uint32 index 列表。
     * 复制系统按桶读取并消费（Drain）。
     *
     * @tparam T 组件类型（编译期分桶键）
     */
    template <typename T>
    class DirtyIndex
    {
    public:
        void Mark(uint32 index)
        {
            // 简化实现：允许重复标记，复制系统去重（或调用方保证不重）
            _dirty.push_back(index);
        }

        /**
         * @brief 取出本帧全部脏 index 并清空
         */
        std::vector<uint32> Drain()
        {
            std::vector<uint32> result;
            result.swap(_dirty);
            return result;
        }

        size_t Count() const { return _dirty.size(); }

    private:
        std::vector<uint32> _dirty;
    };

} // namespace MMO::ECS
```

> **注意**：这是**简化版**。生产级应去重（`std::vector<bool>` 位图 + 列表双结构），
> 但 MVP 顺序（ECS_00 已拍板无 MVP）——直接上生产版：

```cpp
// Src/Common/ECS/DirtyIndex.h（生产版——带去重）
#pragma once

#include <cstdint>
#include <vector>

#include "Common/Core/Types.h"

namespace MMO::ECS
{

    /**
     * @brief 按组件类型分桶的脏实体标记（带 O(1) 去重）
     *
     * _marked 位图（index → bool）保证每实体每帧至多进一次桶；
     * _dirty 列表保序输出。
     */
    template <typename T>
    class DirtyIndex
    {
    public:
        void Mark(uint32 index)
        {
            if (index >= _marked.size())
            {
                _marked.resize(index + 1, false);
            }
            if (!_marked[index])
            {
                _marked[index] = true;
                _dirty.push_back(index);
            }
        }

        std::vector<uint32> Drain()
        {
            for (uint32 i : _dirty)
            {
                _marked[i] = false;
            }
            std::vector<uint32> result;
            result.swap(_dirty);
            return result;
        }

        size_t Count() const { return _dirty.size(); }

    private:
        std::vector<bool>   _marked;
        std::vector<uint32> _dirty;
    };

} // namespace MMO::ECS
```

---

## 4. `Scene` 集成 — 组件 + 脏标记绑定

在 `Scene` 上挂一组 `DirtyIndex`（按组件类型）：

```cpp
// Src/Common/ECS/Scene.h（追加）
#include "Common/ECS/DirtyIndex.h"

// 在 Scene 类内追加：

    // ── 脏标记（复制驱动）──

    /**
     * @brief 标记组件为脏（复制系统消费）
     * @tparam T 组件类型
     * @param entityID 实体
     */
    template <typename T>
    void MarkComponentDirty(uint64 entityID)
    {
        auto e = _registry.Resolve(entityID);
        if (e == entt::null)
        {
            return;
        }
        _dirtyIndex<T>.Mark(static_cast<uint32>(entt::to_integral(e)));
    }

    template <typename T>
    DirtyIndex<T> &GetDirtyIndex() { return _dirtyIndex<T>; }

private:
    // 脏索引集合（按组件类型）
    template <typename>
    friend struct SceneDirtyHelper;

    entt::registry _entt;
    EntityRegistry _registry;
    // 实际存储：见下方 helper（C++ 无模板成员数组，用 friend helper 聚合）
```

> **C++ 限制**：模板成员不能存容器里。解法——用**模板成员函数 + 局部 static** 或
> **CRTP/friend helper**。更干净的做法：`Scene` 持有 `DirtyIndexBag`（见下），
> 复制系统显式 `GetDirtyIndex<Position>()`。

---

## 5. 组件所有权模型（权威）

| 组件 | 写入方 | 读取方 | 复制 |
|---|---|---|---|
| `Position` | Movement 系统（C++） | 复制/AOI/脚本快照 | ✅ dirty |
| `Velocity` | Movement 系统（C++） | Movement | ❌ |
| `Health` | 战斗系统（C++/脚本 Bridge） | 复制/脚本 | ✅ dirty |
| `Tags`（DeadTag 等） | 战斗/生命周期 | 查询过滤 | 部分 |
| `PlayerConn` | EnterWorld（C++） | 复制/离线 | ❌ |
| `AIState` | 脚本 `[game_system]` | 脚本 | ❌（客户端不管 AI） |
| `BattleStats` | 脚本 Bridge | 复制（属性面板）/脚本 | ✅ dirty |

**Dirty 规则**：
1. C++ 系统写组件 → 显式 `MarkComponentDirty<T>(entityID)`
2. 脚本经 Bridge 写回 → Bridge 内部自动 `MarkComponentDirty<T>`
3. 复制系统帧末 `Drain` 脏集 → 打包发送 → 清空

---

## 6. 构建脚本变动

`Src/World/xmake.lua` 的 `add_files("**.cpp")` 已覆盖 `Component/`——无需改。
`Src/Common/ECS/xmake.lua` 的 `add_headerfiles("*.h")` 已覆盖新头文件——无需改。

**注意**：`DirtyIndex` 是 header-only 模板，`Scene.h` 引入后 `CommonECS` 自动带上。

---

## 7. 验证步骤（本篇验收）

```powershell
# 1. 构建
xmake build WorldServer

# 2. 单元级验证（临时测试代码或 Tools/Tests 下新建）
#    - emplace Position + MarkComponentDirty → Drain 返回该实体
#    - 重复 Mark 同一实体 → Drain 只返回一次（去重）
#    - Destroy 实体后再 Mark → 忽略（Resolve 返回 null）
```

**验收标准**：
- [ ] 构建零错误
- [ ] `DirtyIndex` 去重语义正确
- [ ] `Scene::MarkComponentDirty<Position>(invalidID)` 安全（不崩溃）
- [ ] 组件 emplace/remove 通过 `Scene` API 正常工作

---

## 8. 踩坑预警

1. **`DirtyIndex` 的 index 是 EnTT 内部 index 还是 EntityID？**——用 EnTT 内部 index
   （`entt::to_integral`），因为复制系统遍历 EnTT view 时天然拿到它；EntityID 需要
   `Resolve` 开销。**但**注意 EnTT index 与 EntityRegistry index 是**同一套**（都从 0 递增），
   因为 `EntityRegistry` 直接 `entt::entity(index)` 构造——这保证了二者对齐。
2. **`std::vector<bool>` 位图**：`_marked.resize(index+1)` 是 O(1) 摊还，但 `vector<bool>` 的
   `operator[]` 返回 proxy——用 `_marked[index] = true` 没问题，读用 `_marked[index]` 也行。
3. **组件热重载**：脚本改了 `AIState` 的字段布局？——不可能，组件是 **C++ 固定**的。
   脚本只能改"使用组件的逻辑"，不能改组件本身。这是铁律 1 的直接推论。
