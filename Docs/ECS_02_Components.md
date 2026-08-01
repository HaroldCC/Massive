# 组件集与脏索引（ECS_02_Components）

> 本篇给出 P0/P1 的**固定 C++ 组件集**（既有 4 个逐字保留 + 新增 4 个）、取代死代码
> `DirtyTracker<T>` 的 **DirtyIndex**（组件类型 × 实体粒度的脏标记，挂在 Scene 上），以及把现有
> 头注释「脚本只读不写」升级为**可执行的组件所有权契约**。
>
> - 权威契约见 `ECS_00_Overview.md` §3（命名/格式）、§6.4（组件集决策）、§4（8 阶段管线与复制屏障）。
> - uint64 `EntityID`、`EntityRegistry`、`Resolve()` 定义在 `ECS_01_Identity.md`，本篇只引用不重定义。
> - `Grid`/`GridCell` 的填充逻辑、`VisibleSet` 的 AOI 增量算法见 `ECS_04_SpatialAOI.md`。
> - `entity_type` 派生、`REPLICATE_COMPONENT` codegen、dirty-driven 打包见 `ECS_05_Replication.md`。
> - 脚本侧 `Bridge_Set*` 如何汇入唯一写入点见 `ECS_06_ScriptBridge.md`。
>
> 本篇所有代码可直接照抄，无占位、无 TODO。所有 EnTT 调用对齐仓库内 **EnTT 3.16.0**
> （`ThirdParty/entt/src`）。

---

## 1. 组件集总览（契约 ECS_00 §6.4）

P0~P3 只固定下面 8 个数据组件 + 8 个 tag。其余（Inventory/Buff/Cooldown/Threat/Faction…）
按玩法迭代再加，不进 P0（YAGNI，见 ECS_00 §6.4）。

| 组件 | 文件 | 状态 | 写者（唯一） | 复制？ | 进 DirtyIndex？ | 脚本访问 |
|---|---|---|---|---|---|---|
| `Position` | `Src/World/Component/Position.h` | 保留 | Movement（C++） | 是 | 是 | 只读 |
| `Velocity` | `Src/World/Component/Velocity.h` | 保留 | Movement（C++） | 见 ECS_05 | 是 | 只读 |
| `Health` | `Src/World/Component/Health.h` | 保留 | Combat（C++/脚本事件） | 是 | 是 | 读写 |
| `BattleStats` | `Src/World/Component/BattleStats.h` | 保留 | Combat/Buff（C++） | 部分（见 ECS_05） | 是 | 只读 |
| `NetId` | `Src/World/Component/NetId.h` | 新增 | 生成时（EntityRegistry） | 否（本身是 diff 主键） | 否（不可变） | 只读 |
| `GridCell` | `Src/World/Component/GridCell.h` | 新增 | SpatialIndex（C++） | 否（服务器内部） | 否 | 只读 |
| `VisibleSet` | `Src/World/Component/VisibleSet.h` | 新增（玩家专有） | AOI（C++） | 否（服务器内部） | 只读 | 只读 |
| `PlayerTag` `MonsterTag` `NpcTag` `ItemTag` | `Src/World/Component/Tags.h` | 补齐 | 生成时（C++） | 派生为 `entity_type`（ECS_05） | 结构变更（见 §3.3） | 只读 |
| `DeadTag` `CombatTag` `StunnedTag` `DormantTag` | `Src/World/Component/Tags.h` | 保留+新增 | Combat/脚本/PostUpdate（C++） | `DeadTag` 派生（见 ECS_05） | 结构变更 | 只读 |

> **约定**：所有「脚本只读」组件，脚本侧只能经 `Bridge_Get*` 取**值拷贝**（铁律 1，ECS_00 §1）；
> 所有「脚本读写」组件的写路径**必须**经 §3.3 的 `Scene::WriteComponent<T>` 唯一入口，绝不允许脚本
> 直接持有 `T&`。这条线由 §4 的所有权表 + 头注释共同强制。

### 1.1 既有组件（逐字保留，不改一字）

`Position` / `Velocity` / `Health` / `BattleStats` 四个头文件保持现状（已符合 CodingStandard v2.0：
`struct` 纯数据、`camelCase` 成员、定宽整数、中文 Doxygen）。仅在 §4 收紧其**头注释里的所有权语义**
（把「脚本只读不写」从口头约定变成契约）。此处不重复贴出，内容见各文件；四者的定宽整数与默认值
均已核实合规。

> **已知冗余（记录，不在本篇修）**：`BattleStats.currentHp/maxHp` 与 `Health.current/max` 语义重叠。
> 约定 `Health` 为生命值的**唯一复制真相源**，`BattleStats` 的 HP/MP 字段视为战斗计算的只读快照，
> 后续玩法迭代应下线 `BattleStats` 中的 HP/MP（YAGNI，不进 P0）。

### 1.2 新增组件

三个新数据组件。均置于顶层 `namespace MMO`（对齐真实布局，见 ECS_00 §3.2），文件头照
`IOContextPool.h` 范式。

**`Src/World/Component/NetId.h`** —— `entt::entity` → uint64 的正向回查（反向 uint64 →
`entt::entity` 由 `EntityRegistry` 持有，见 ECS_01）。热路径（复制打包、脏标记）在遍历 EnTT
view 拿到 `entt::entity` 后，需要它对应的稳定 uint64 句柄，`NetId` 就是这份内联缓存，避免每次
反查映射表。

```cpp
/**
 * @file NetId.h
 * @brief 实体的稳定网络句柄（uint64 EntityID）Component
 *
 * EnTT 的 entt::entity 是 scene 内部存储句柄，绝不外泄（ECS_00 §3.1）。
 * NetId 把生成时分配的 uint64 EntityID 内联缓存在实体上，
 * 供复制打包 / DirtyIndex / AOI 事件在遍历 view 时零反查地取得对外句柄。
 * 生成时由 EntityRegistry 写入，此后不可变；脚本只读。
 */
#pragma once

#include "Common/Core/Types.h"
#include "Common/ECS/EntityID.h"

namespace MMO
{

    /**
     * @brief 实体的稳定网络句柄
     *
     * netID 高 16 位含 scene，低位含 index+version（布局见 ECS_00 §3.1）。
     * 生成后不可变，故不进 DirtyIndex。
     */
    struct NetId
    {
        ECS::EntityID netID = ECS::kInvalidEntityID;
    };

} // namespace MMO
```

**`Src/World/Component/GridCell.h`** —— 记录实体当前所在的均匀格子线性下标（`Grid`，ECS_04）。
`SpatialIndex` 阶段在实体跨格时更新；`-1` 表示尚未入索引。

```cpp
/**
 * @file GridCell.h
 * @brief 实体在均匀格子空间索引中的所在格 Component
 *
 * cellIndex 为 Grid（见 ECS_04）的线性格下标；-1 表示尚未入索引。
 * 仅由 SpatialIndex 阶段写入（实体跨格才改）；服务器内部数据，不复制；脚本只读。
 */
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 实体所在格线性下标
     */
    struct GridCell
    {
        int32 cellIndex = -1;
    };

} // namespace MMO
```

**`Src/World/Component/VisibleSet.h`** —— **玩家实体专有**，保存「上一 tick 该玩家可见的实体
uint64 集合」。AOI（ECS_04）每 tick 用它与新可见集做差得到 enter/leave 事件；复制打包（ECS_05）
用它决定给该玩家发哪些实体的 diff。

存储决策 —— **升序去重的扁平 `std::vector<EntityID>`**（不用 `unordered_set`）：

- AOI 的核心操作是「本 tick 可见集」与「上 tick 可见集」的**有序集合差**（算 enter/leave）。两个升序
  vector 做一趟归并即得对称差，`O(n+m)` 且顺序访问、缓存友好、零哈希、零节点分配。
- 同屏上千的量级下，vector 全程复用容量（`clear()` 不还内存），每 tick 无堆分配；`unordered_set`
  的桶遍历顺序不定、无法归并、且每元素一次哈希+可能 rehash，反而更慢更占内存。
- 玩家数远小于实体数，只有玩家实体挂 `VisibleSet`，总内存受控。

```cpp
/**
 * @file VisibleSet.h
 * @brief 玩家可见实体集合 Component（玩家实体专有）
 *
 * 保存上一 tick 该玩家可见的实体 uint64 句柄，升序去重存放。
 * 由 AOI 阶段（见 ECS_04）每 tick 用「新可见集 vs 本集」的有序集合差算出
 * enter/leave 事件后原地更新；复制打包（见 ECS_05）据此决定发送范围。
 * 服务器内部数据，不直接复制；脚本只读。
 *
 * 存储用扁平 vector 而非 hash set：AOI 的热操作是两个升序集合的差集归并，
 * vector 一趟归并即得，缓存友好、复用容量、每 tick 零堆分配。
 */
#pragma once

#include <vector>

#include "Common/Core/Types.h"
#include "Common/ECS/EntityID.h"

namespace MMO
{

    /**
     * @brief 玩家可见实体集合（升序去重）
     *
     * @note 视野半径不入本组件——它是场景级参数，来自 SceneConfig（见 ECS_04）。
     */
    struct VisibleSet
    {
        std::vector<ECS::EntityID> visible; // 升序去重；AOI 每 tick 原地更新
    };

} // namespace MMO
```

> **与现有 `Src/World/System/System.h` 里 `VisibleSet` 的差异（务必对齐）**：旧定义是
> `struct VisibleSet { std::vector<uint32_t> entityIDs; float viewRadiusXZ; float viewRadiusY; }`，
> 且不是组件、而是 `SystemAOI` 的输出参数 `unordered_map<uint32,VisibleSet>` 里的值。本篇把它
> **升级为玩家实体上的真组件**、id 宽度 `uint32→uint64 EntityID`、**移除半径字段**（半径是场景级
> 参数，见 SceneConfig）。旧 `System.h`（连同 `SystemAOI`/`RunCPPSystems`）在 ECS_03 被
> `StageScheduler` 取代，其 `VisibleSet` 定义随之删除（见 §5 迁移清单）。

### 1.3 Tags.h 重写

补齐 ECS_00 §6.4 要求的 `NpcTag`/`ItemTag`/`DormantTag`，保留原有 5 个 tag。全部为
size=0 空 `struct`（EnTT 空类型不占存储）。

```cpp
/**
 * @file Tags.h
 * @brief 实体状态 Tag Component（零存储零开销纯标记）
 *
 * 每个 Tag 各 1 个空 struct（size=0），EnTT 空类型不占存储空间。
 * 类型 tag（Player/Monster/Npc/Item）互斥，生成时写入、生命周期内不变，
 * 复制时由 EntityTypeFromTags 派生为网络 entity_type（见 §2 与 ECS_05）。
 * 状态 tag（Dead/Combat/Stunned/Dormant）由 Combat/脚本事件/PostUpdate 增删。
 * 脚本经 Bridge_* 查询，不直接增删。
 */
#pragma once

namespace MMO
{

    /**
     * @brief 实体为玩家
     */
    struct PlayerTag
    {
    };

    /**
     * @brief 实体为怪物
     */
    struct MonsterTag
    {
    };

    /**
     * @brief 实体为 NPC（可交互，非战斗单位）
     */
    struct NpcTag
    {
    };

    /**
     * @brief 实体为地面掉落物 / 可拾取物
     */
    struct ItemTag
    {
    };

    /**
     * @brief 实体已死亡（currentHp == 0）
     */
    struct DeadTag
    {
    };

    /**
     * @brief 实体处于战斗状态
     */
    struct CombatTag
    {
    };

    /**
     * @brief 实体被眩晕/定身
     */
    struct StunnedTag
    {
    };

    /**
     * @brief 实体休眠——移出活跃集，不参与移动积分/AOI（PostUpdate 判定）
     *
     * 兑现 ECS_00 铁律 2 的「分频错峰」：远离所有玩家的实体打上 DormantTag，
     * 热循环遍历活跃集时经 entt::exclude<DormantTag> 跳过（见 ECS_04 活跃集）。
     */
    struct DormantTag
    {
    };

} // namespace MMO
```

---

## 2. 废弃 `EEntityType`：`entity_type` 由 tag 派生

`Src/World/Component/EntityType.h` 将被**废弃**（ECS_00 §6.4），但**删除动作推迟到 P3/`ECS_05`**——
它的消费者 `WorldServer::SystemReplicate`（`WorldServer.cpp:13/694/698`）此刻仍编译，本篇删会断构建
（详见 §5「删除时序铁律」）。它的问题：`EEntityType` 是一份与 tag 集平行、需要手动同步的第二真相源——
实体既有 `PlayerTag` 又得记 `ENTITY_PLAYER`，二者可漂移。既然类型信息已经在互斥的类型 tag 里，网络需要的
`entity_type` 字段应在**复制打包时按 tag 派生**，消除冗余。本篇先**新增**派生 helper（新文件，与旧枚举并存），
P3 复制重写时把 `SystemReplicate` 改调它、再删旧枚举头。

派生 helper（无副作用的纯查询）落在复制层 `Src/World/Replication/`（ECS_05 拥有），本篇给出完整
实现以定契约。数值**沿用旧 `EEntityType`**（`PLAYER=1/NPC=2/MONSTER=3`）保证 wire 兼容，新增
`ITEM=4`；协议字段仍是 `Proto::EntitySpawnNtf.entity_type`（int32），打包时 `static_cast<int32>`。

```cpp
/**
 * @file EntityType.h
 * @brief 由类型 tag 派生网络 entity_type（取代已删除的 EEntityType 枚举）
 *
 * entity_type 不再是独立存储的枚举，而是复制打包时按互斥类型 tag 现算，
 * 消除「tag 集 vs 枚举」双真相源漂移。数值沿用旧 EEntityType 以兼容 wire。
 */
#pragma once

#include <entt/entt.hpp>

#include "Common/Core/Types.h"
#include "World/Component/Tags.h"

namespace MMO
{

    /** @name 网络实体类型常量（对齐旧 EEntityType，追加 Item） */
    /** @{ */
    inline constexpr uint32 kEntityTypeUnknown = 0;
    inline constexpr uint32 kEntityTypePlayer  = 1;
    inline constexpr uint32 kEntityTypeNpc     = 2;
    inline constexpr uint32 kEntityTypeMonster = 3;
    inline constexpr uint32 kEntityTypeItem    = 4;
    /** @} */

    /**
     * @brief 按互斥类型 tag 派生网络 entity_type
     *
     * 类型 tag 互斥，正常实体至多命中一个；优先级 Player>Npc>Monster>Item 仅为兜底。
     * @param reg  实体所在 registry
     * @param e    entt::entity（scene 内部句柄）
     * @return 网络 entity_type，无类型 tag 返回 kEntityTypeUnknown
     */
    inline uint32 EntityTypeFromTags(const entt::registry &reg, entt::entity e)
    {
        if (reg.all_of<PlayerTag>(e))
        {
            return kEntityTypePlayer;
        }
        if (reg.all_of<NpcTag>(e))
        {
            return kEntityTypeNpc;
        }
        if (reg.all_of<MonsterTag>(e))
        {
            return kEntityTypeMonster;
        }
        if (reg.all_of<ItemTag>(e))
        {
            return kEntityTypeItem;
        }
        return kEntityTypeUnknown;
    }

} // namespace MMO
```

`reg.all_of<T>(e)` 的签名已核实（EnTT 3.16.0，单类型返回 `bool`）。复制打包在生成
`EntitySpawnNtf` 时调 `EntityTypeFromTags(reg, e)`（见 ECS_05）。

---

## 3. DirtyIndex —— 取代死代码 `DirtyTracker<T>`

`Src/Common/ECS/DirtyTracker.h` 是全仓零引用的死模板（grep 确认：无实例化、无 `#include`、无调用），
**删除**。取而代之的是挂在 `Scene` 上、按**组件类型 × 实体**粒度做脏标记的单一 `DirtyIndex`。

### 3.1 设计

- **粒度**：`(组件类型, EntityID)`。同一实体的 `Position` 脏和 `Health` 脏互不相干，复制层按每个
  已注册复制的组件类型分别取脏集（ECS_05 的 `REPLICATE_COMPONENT` 决定注册哪些类型）。
- **键**：组件类型用 `entt::type_hash<T>::value()`（`id_type` = `uint32`，EnTT 3.16.0 已核实——
  `registry.storage<T>` 默认参数即 `type_hash<T>::value()`）。这与 EnTT 自身给组件存储分配的 id
  同源，语义一致。
- **值**：每类型一个 `std::unordered_set<EntityID>`（uint64，天然去重——同一 tick 内一实体被同一
  系统改多次只记一次）。
- **归属**：`DirtyIndex` 是跨服务 ECS 基建，随 `Scene` 放 `Src/Common/ECS/`（target `CommonECS`，
  ECS_00 §2）。`Scene` 持有一个 `DirtyIndex _dirtyIndex` 成员。
- **生命周期**（对齐 ECS_00 §4 的 8 阶段）：阶段 1~7 串行标记（无锁）；阶段 8 每玩家打包任务
  **并行只读**遍历脏集（所有玩家读同一份，非破坏性）；全部玩家打包完，在屏障处**统一** `ClearAll()`
  （保证不漏发）。因此不做「读即清」的 per-drain，而是「并行读 → 屏障清」。

### 3.2 实现 `Src/Common/ECS/DirtyIndex.h`

```cpp
/**
 * @file DirtyIndex.h
 * @brief 组件类型 × 实体 粒度的脏标记索引（取代已删除的 DirtyTracker）
 *
 * 挂在 Scene 上。模拟阶段（ECS_00 §4 阶段 1~7）串行标记，复制阶段（阶段 8）
 * 每玩家打包任务并行只读遍历，屏障处统一 ClearAll。
 * 键为 entt::type_hash<T> 分配的组件类型 id（uint32），值为该类型的脏 EntityID 集。
 */
#pragma once

#include <unordered_map>
#include <unordered_set>

#include <entt/core/type_info.hpp>

#include "Common/Core/Types.h"
#include "Common/ECS/EntityID.h"

namespace MMO::ECS
{

    /**
     * @brief 组件类型 × 实体 粒度的脏标记索引
     *
     * @warning Mark 只允许在模拟阶段（串行）调用；Dirty 的并行只读遍历只允许在
     *          复制阶段（阶段 8）；ClearAll 只允许在复制屏障（单线程）调用。
     */
    class DirtyIndex
    {
    public:
        using DirtySet = std::unordered_set<EntityID>;

        /**
         * @brief 标记某实体的组件 T 为脏
         * @tparam T  组件类型
         * @param entityID  uint64 实体句柄
         */
        template <typename T>
        void Mark(EntityID entityID)
        {
            _dirtyByType[entt::type_hash<T>::value()].insert(entityID);
        }

        /**
         * @brief 取组件 T 的脏集（非破坏性，供复制阶段并行只读）
         * @tparam T  组件类型
         * @return 该类型脏 EntityID 集的常引用；无脏时返回空集
         */
        template <typename T>
        const DirtySet &Dirty() const
        {
            static const DirtySet kEmpty;
            auto it = _dirtyByType.find(entt::type_hash<T>::value());
            if (it == _dirtyByType.end())
            {
                return kEmpty;
            }
            return it->second;
        }

        /**
         * @brief 查询某实体的组件 T 是否脏
         * @tparam T  组件类型
         */
        template <typename T>
        bool IsDirty(EntityID entityID) const
        {
            auto it = _dirtyByType.find(entt::type_hash<T>::value());
            return it != _dirtyByType.end() && it->second.contains(entityID);
        }

        /**
         * @brief 清空全部脏集（复制屏障统一调用）
         *
         * 只清集合元素、保留 map 桶与各集合容量，跨 tick 复用内存零重分配。
         */
        void ClearAll()
        {
            for (auto &[typeId, set] : _dirtyByType)
            {
                [[maybe_unused]] uint32 id = typeId;
                set.clear();
            }
        }

    private:
        std::unordered_map<uint32, DirtySet> _dirtyByType;
    };

} // namespace MMO::ECS
```

> **并发正确性**：阶段 8 多线程只调 `const` 的 `Dirty<T>()`（`static const kEmpty` 的初始化线程安全，
> C++11 起有保证），期间无任何 `Mark`/`ClearAll`，故对 `_dirtyByType` 只读无数据竞争。复制层
> 只对**已注册复制**的类型逐一调 `Dirty<Position>()`/`Dirty<Health>()`…（类型集在 ECS_05 编译期已知），
> 无需类型擦除遍历。

### 3.3 写入统一入口 `Scene::WriteComponent<T>`（推荐路径）

**决策：显式访问器，而非 `on_update` 观察者。** 让 C++ 系统与脚本 `Bridge_Set*` 都经**同一个**
标脏点，杜绝「改了组件忘标脏」。在 `Scene` 上新增：

```cpp
// 追加到 Src/Common/ECS/Scene.h 的 Scene 类中

/**
 * @brief 组件写入唯一入口——改值并标脏（读写组件必须走此路径）
 * @tparam T   组件类型
 * @tparam Fn  形如 void(T&) 的修改器
 * @param entityID  uint64 实体句柄
 * @param mutate    对组件的原地修改
 * @return 修改后的组件引用
 *
 * @note 解析 uint64 -> entt::entity 由 EntityRegistry 提供（Resolve，见 ECS_01）。
 *       脚本读写组件经 Bridge_Set* 汇入此入口（见 ECS_06），与 C++ 共用一个标脏点。
 */
template <typename T, typename Fn>
T &WriteComponent(EntityID entityID, Fn &&mutate)
{
    entt::entity e = Resolve(entityID); // ECS_01 EntityRegistry：uint64 -> entt::entity
    MASSIVE_ASSERT(_registry.all_of<T>(e), "WriteComponent: 实体缺少该组件");
    T &comp = _registry.get<T>(e);
    std::forward<Fn>(mutate)(comp);
    _dirtyIndex.Mark<T>(entityID);
    return comp;
}

/**
 * @brief DirtyIndex 访问器（复制层只读遍历 / 系统标脏用）
 */
DirtyIndex &Dirty()
{
    return _dirtyIndex;
}

const DirtyIndex &Dirty() const
{
    return _dirtyIndex;
}
```

对应地在 `Scene` 私有区新增成员（并 `#include "Common/ECS/DirtyIndex.h"`）：

```cpp
private:
    uint32         _sceneId;
    entt::registry _registry;
    DirtyIndex     _dirtyIndex;
```

调用示例——脚本对 `Health` 扣血（读写组件），经 Bridge 汇入唯一入口：

```cpp
// ECS_06 的 Bridge_ApplyDamage 内部（脚本 -> C++）
scene.WriteComponent<Health>(targetID,
    [dmg](Health &hp)
    {
        hp.current = (hp.current > dmg) ? (hp.current - dmg) : 0;
    });
// -> 自动 Mark<Health>(targetID)，复制层下 tick 取脏发送
```

**热路径特例（Position/Velocity）**：Movement 每 tick 对**整个活跃集**积分，逐实体套
`WriteComponent` 的 lambda 转发+断言不划算。约定 Movement 直接遍历 EnTT view 改 `Position`，
并在同一循环里对该实体标脏（`NetId` 内联缓存直接给出 uint64，零反查）：

```cpp
// ECS_03 Movement 阶段（示意；完整调度见 ECS_03）
auto view = scene.Registry().view<Position, const Velocity, const NetId>(
    entt::exclude<DeadTag, DormantTag>);
auto &dirty = scene.Dirty();
for (auto [e, pos, vel, net] : view.each())
{
    pos.x += vel.vx * dt;
    pos.y += vel.vy * dt;
    pos.z += vel.vz * dt;
    dirty.Mark<Position>(net.netID);
}
```

对活跃集的 5w 量级，`unordered_set` 插入摊还 O(1)、亚毫秒。因 Position 对活跃实体「几乎恒脏」，
ECS_05 可选择对其特判（直接以活跃集为脏源）——本篇不预设，接口两条路都支持。

**为何不用 `on_update` 观察者**（已核实签名，仅供对照）：

```cpp
// EnTT 3.16.0：三个 sink，监听器签名均为 void(entt::registry&, entt::entity)
registry.on_update<Position>().connect<&OnDirty>();       // patch/replace 后触发
registry.on_construct<Position>().connect<&OnAdd>();       // emplace 后触发
registry.on_destroy<Position>().connect<&OnRemove>();      // remove/erase 前触发
```

`on_update` **仅在 `patch<T>`/`replace<T>` 后触发**，而热路径 `pos.x += …`（`get<T>&` 原地改）
**不会触发**——若靠观察者，就得强制 5w 实体每帧走 `patch<T>`（每实体一次 lambda + 信号派发开销），
且回调里还得反查 `NetId` 才拿得到 uint64。故**热路径否决观察者**，用显式标脏。

> **观察者仍有一处用途**：**结构性增删** tag（`PlayerTag`/`DeadTag` 等的 add/remove 影响派生
> `entity_type` 与 spawn/despawn），可用 `on_construct`/`on_destroy` 统一记「本 tick 新增/移除实体」
> 供复制发 spawn/despawn（见 ECS_05）。数据组件的值变更一律走 `WriteComponent`/显式 `Mark`。

### 3.4 复制屏障与 ClearAll

对齐 ECS_00 §4：阶段 8 所有玩家打包任务**并行只读** `scene.Dirty().Dirty<T>()`；全部完成后，
在屏障处**单线程**调一次 `scene.Dirty().ClearAll()`。绝不在某个玩家打包完就清——否则后续玩家漏发。
`ClearAll` 只清元素保留容量，跨 tick 零重分配。

---

## 4. 组件所有权模型（可执行契约）

把现有头注释里的「脚本只读不写」从口头约定升级为**每组件唯一写者 + 复制/脏语义**的契约。§1 总览表
是索引，本节是规则本体，头注释是落地强制点（每个组件头的类 Doxygen 必须写明「写者」与「脚本读/写」）。

| 组件 | 唯一写者 | 写入方式 | 复制？ | DirtyIndex？ | 脚本 |
|---|---|---|---|---|---|
| `Position` | Movement（C++，阶段 3） | view 内原地改 + `Mark<Position>` | 是 | 是 | 只读（`Bridge_GetPosition` 取拷贝） |
| `Velocity` | Movement（C++，据移动意图） | 同上 | 见 ECS_05 | 是 | 只读 |
| `Health` | Combat（C++ / 脚本伤害事件） | `WriteComponent<Health>` | 是 | 是 | **读写**（经 Bridge 汇入 `WriteComponent`） |
| `BattleStats` | Combat/Buff（C++） | `WriteComponent<BattleStats>` | 部分（见 ECS_05） | 是 | 只读（全量取拷贝做计算） |
| `NetId` | EntityRegistry（生成时，C++） | 生成时 emplace，此后不可变 | 否（是 diff 主键） | 否 | 只读 |
| `GridCell` | SpatialIndex（C++，阶段 4） | 跨格时 `WriteComponent`/直接改 | 否（内部） | 否 | 只读 |
| `VisibleSet` | AOI（C++，阶段 5） | 原地归并更新 | 否（内部，驱动打包范围） | 否 | 只读 |
| 类型 tag（Player/Monster/Npc/Item） | 生成时（C++） | 生成时 emplace，生命周期不变 | 派生 `entity_type`（ECS_05） | 结构变更经 `on_construct` | 只读 |
| 状态 tag（Dead/Combat/Stunned） | Combat / 脚本事件（C++） | `emplace`/`remove` | `DeadTag` 派生（ECS_05） | 结构变更经观察者 | 只读（`Bridge_Has*` 查询） |
| `DormantTag` | PostUpdate（C++，阶段 7） | 休眠判定 `emplace`/`remove` | 否 | 否 | 只读 |

**强制规则**：

1. **唯一写者**：每个组件只有一个系统写。跨系统改同一组件（如脚本要改 `Position`）一律禁止——
   脚本表达移动意图，由 Movement 落值。头注释「脚本只读不写」即此律。
2. **读写组件（仅 `Health` + 战斗相关）**必须经 `Scene::WriteComponent<T>`；脚本经 `Bridge_Set*`
   转调同一入口（ECS_06），保证标脏。
3. **只读组件**脚本只能取**值拷贝**（`Bridge_Get*`），永不持 `T&`（铁律 1：组件数据不进 das heap）。
4. **复制列**的最终形态由 ECS_05 的 `REPLICATE_COMPONENT` 声明确定；本表给出默认意图，标「见 ECS_05」
   者以 ECS_05 为准。
5. **头注释即契约**：新增/重写的组件头（§1.2、§1.3）已在类 Doxygen 写明写者与脚本读/写；既有四组件
   在 §1.1 收紧其注释语义。

---

## 5. 迁移步骤清单（可执行）

按顺序执行；每步给出验证方法。P0/P1 内完成（依赖 ECS_01 的 `EntityID`/`EntityRegistry`/`Resolve`）。

> **删除时序铁律（务必遵守）**：`EntityType.h`（`EEntityType`）与 `System.h` 里的旧 `VisibleSet` **不能在本篇删除**。
> 真实源码里它们的消费者 `WorldServer::SystemReplicate`（`WorldServer.cpp:637`）**仍被编译**（`WorldServer.cpp:13`
> `#include "World/Component/EntityType.h"`；`:694/:698` 用 `EEntityType::ENTITY_MONSTER/ENTITY_PLAYER`；
> `:637-639` 形参 `const std::unordered_map<uint32, VisibleSet>&` 且 `:707` 迭代 `vs.entityIDs`）。
> **死调用点 ≠ 不编译**——只要 `SystemReplicate` 这个 TU 还在，删这两样就会断 `WorldServer` 构建。
> 二者的删除随 `SystemReplicate` 一起在 **P3/`ECS_05`**（复制系统重写、旧 `SystemReplicate` 退休）执行。
> 本篇只**新增**，不删这两样；`DirtyTracker.h`（真·零引用）与 `RegisterScriptComponent`（无定义死声明）可安全删。

1. **删除真·死代码（安全，零引用）**
   - 删 `Src/Common/ECS/DirtyTracker.h`（全仓零引用，grep 确认）。
   - **不删** `Src/World/Component/EntityType.h`——推迟到 P3/`ECS_05`（见上「删除时序铁律」）。本篇它继续存在，
     新旧 `EntityType` 暂时并存（旧 `World/Component/EntityType.h` 供 `SystemReplicate`；新
     `World/Replication/EntityType.h` 的 `EntityTypeFromTags` 供 P3 复制打包，见步骤 4）。
   - 验证：`grep -rn "DirtyTracker" Src/` 为 0（`DirtyTracker.h` 已删且本无引用）。

2. **新增组件头**
   - 建 `Src/World/Component/NetId.h`、`GridCell.h`、`VisibleSet.h`（§1.2 逐字，文件名/类型名与旧
     `System.h::VisibleSet` **不冲突**——新的是 `World/Component/VisibleSet.h` 里的组件，旧的仍在
     `System.h` 命名空间 `MMO` 下，暂共存；消费者迁移到新组件在 P2/`ECS_04` 的 AOI 重写，旧 `System.h`
     的 `VisibleSet` 随 `SystemReplicate` 在 P3 删除）。
   - 重写 `Src/World/Component/Tags.h`（§1.3，补 `NpcTag`/`ItemTag`/`DormantTag`）。
   - 验证：编译 `WorldServer` target 通过；`static_assert(std::is_empty_v<MMO::DormantTag>)` 等
     四个新 tag 均为空类型。
   - **命名冲突排查**：新 `World/Component/VisibleSet.h` 定义 `MMO::VisibleSet` 与旧 `System.h` 的
     `MMO::VisibleSet` **同名同命名空间会 ODR 冲突**。P1/P2 期间两者不能同时被同一 TU 见到。做法：
     新组件先命名为 `MMO::AoiVisibleSet`（临时），待 P3 删旧 `System.h::VisibleSet` 后，再在 `ECS_05`
     的迁移里统一重命名回 `VisibleSet`。（若你选择在 P2 就退休 `System.h`，则可直接用 `VisibleSet` 无需临时名。）

3. **新增 DirtyIndex 并接入 Scene**
   - 建 `Src/Common/ECS/DirtyIndex.h`（§3.2 逐字，target `CommonECS`）。
   - `Scene.h` 加 `#include "Common/ECS/DirtyIndex.h"`、`#include <utility>`（`std::forward`）、
     `#include "Common/Core/MassiveAssert.h"`（`MASSIVE_ASSERT`）、`DirtyIndex _dirtyIndex` 成员、
     `WriteComponent<T>`/`Dirty()` 方法（§3.3）。`WriteComponent` 内的 `Resolve(entityID)` 是
     `Scene` 自己的成员（转调 `_entityRegistry.Resolve`，见 ECS_01 §3.1）——确保 ECS_01 已给 `Scene`
     加了 `Resolve(EntityID)->entt::entity`。`RegisterScriptComponent` 死声明由 ECS_01 §3.3 删除，本篇不重复。
   - 验证：编译通过；单测 `DirtyIndex_Mark_Dirty_ClearAll`——
     `Mark<Position>(id)` 后 `Dirty<Position>().contains(id)` 为真、`Dirty<Health>()` 为空、
     `ClearAll()` 后两者皆空且再次 `Mark` 不触发重分配（观察容量）。

4. **落地 EntityTypeFromTags（新增，不删旧）**
   - 建 `Src/World/Replication/EntityType.h`（§2 逐字，`EntityTypeFromTags(reg, e)->uint32`）。
     这是**新增文件**，与旧 `World/Component/EntityType.h` 并存；P3 `ECS_05` 重写复制时，`SystemReplicate`
     的 `set_entity_type(static_cast<uint32>(EEntityType::...))`（`:694/:698`）改调 `EntityTypeFromTags`，
     然后才删旧 `World/Component/EntityType.h` + `WorldServer.cpp:13` 的 include。
   - 验证：编译通过；单测 `EntityTypeFromTags`——分别 emplace `PlayerTag`/`NpcTag`/`MonsterTag`/
     `ItemTag`/无 tag 的实体，返回值依次为 `1/2/3/4/0`。

5. **接入写入契约（P1，随 Movement/Combat 上线）**
   - Movement 阶段按 §3.3 热路径示意标脏 `Position`（`view<..., const NetId>` + `Mark<Position>`）。
   - `Health` 的一切写路径改走 `WriteComponent<Health>`；`Bridge_Set*`（ECS_06）转调之。
   - 复制阶段 8 结束后在屏障调 `scene.Dirty().ClearAll()`（ECS_05 接线）。
   - 验证：集成测——造若干移动实体跑一 tick，`Dirty<Position>()` 含全部活跃移动实体；复制打包读脏
     发送后 `ClearAll`，下一 tick 无残留脏（回归「漏发/重发」）。

> **验证总纲**：结构性检查用编译（新头齐全、无悬空声明）；语义检查用上述单测（DirtyIndex 三态、
> EntityTypeFromTags 五分支）；行为检查用一 tick 集成测（脏集正确、屏障清理不漏发）。全绿即本篇交付。
