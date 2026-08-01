# 身份系统（ECS_01_Identity）

> 本篇是实体层重构 **P0 清理地基** 阶段的可照抄实现：`uint64 EntityID` 契约落地、
> per-Scene 的 `EntityRegistry`（index 28 位 + version 20 位自管回收）、`Scene` 改造
> （删链接期地雷 `RegisterScriptComponent`）、玩家 identity 归位到组件、prefab 组合层接口。
>
> 权威契约见 `ECS_00_Overview.md` §3.1（uint64 布局）/ §6（5 项已定决策）——本篇逐字实现，
> **不重复推导**。组件集见 `ECS_02`，调度见 `ECS_03`，prefab 落地实现见 `ECS_08`。
> 所有 EnTT 调用均对 `ThirdParty/entt`（EnTT **3.16.0**）源码核实。

---

## 0. 本篇改动总览

| 项 | 旧（现状源码） | 新（P0 交付） |
|---|---|---|
| 身份标识 | `struct Entity{uint32 sceneId; uint32 entityId}`（`Entity.h`） | `uint64 EntityID`（`EntityID.h`，scene16/index28/version20） |
| version 校验 | 无——`entt::entity` 的 uint32 直接外泄 | `EntityRegistry` 自管 20 位 version，回收即失配 |
| 实体解析 | `Scene::IsValid(Entity)` 仅 `registry.valid` | `Scene::Resolve(EntityID)->entt::entity`，version 校验 |
| `RegisterScriptComponent` | `Scene.h:206` 声明存在、无定义无调用（链接期地雷） | **删除** |
| `DirtyTracker.h` | 零引用死代码 | **删除**（复制脏标记由 `ECS_02` 的 `DirtyIndex` 承担） |
| 玩家 identity | `WorldSession::entity`（跨表结构体） | `WorldSession::entityID`（uint64）+ 实体上的 `PlayerConn{sessionID}` 组件 |
| prefab | 无 | `Bridge_Spawn` 接口 + daslang 配方表形态（实现见 `ECS_08`） |

---

## 1. `EntityID.h`——uint64 身份契约（逐字实现 ECS_00 §3.1）

新建 `Src/Common/ECS/EntityID.h`。这是 `ECS_00` §3.1 的权威常量的**逐字落地**，
不得改动任何位宽或掩码。

```cpp
/**
 * @file EntityID.h
 * @brief uint64 实体标识符——scene(16) / index(28) / version(20) 位布局
 *
 * 全系统最底层契约：网络 ID、脚本句柄、DB 回写、复制 diff 全用它。
 *   63          48 47                      20 19                  0
 *  ┌──────────────┬──────────────────────────┬────────────────────┐
 *  │  scene (16)  │        index (28)         │    version (20)    │
 *  └──────────────┴──────────────────────────┴────────────────────┘
 * 高位放 scene → Resolve(id) 无需先知道 sceneID；version 回收即 ++，旧句柄比对即失效。
 */
#pragma once

#include "Common/Core/Types.h"

namespace MMO::ECS
{

    /** @brief 实体标识符（纯整数，可安全跨 C++/脚本/网络/DB 边界传递） */
    using EntityID = uint64;

    inline constexpr uint64 kSceneBits   = 16;
    inline constexpr uint64 kIndexBits   = 28;
    inline constexpr uint64 kVersionBits = 20;

    inline constexpr uint64 kSceneShift  = kIndexBits + kVersionBits;        // 48
    inline constexpr uint64 kIndexShift  = kVersionBits;                     // 20
    inline constexpr uint64 kVersionMask = (uint64(1) << kVersionBits) - 1;  // 0xFFFFF
    inline constexpr uint64 kIndexMask   = (uint64(1) << kIndexBits) - 1;    // 0xFFFFFFF
    inline constexpr uint64 kSceneMask   = (uint64(1) << kSceneBits) - 1;    // 0xFFFF

    /** @brief 无效实体哨兵（scene=0 保留为无效） */
    inline constexpr EntityID kInvalidEntityID = 0;

    /** @brief 组装 EntityID */
    inline constexpr EntityID MakeEntityID(uint16 scene, uint32 index, uint32 version)
    {
        return (uint64(scene) << kSceneShift) | (uint64(index & kIndexMask) << kIndexShift)
             | (uint64(version) & kVersionMask);
    }

    /** @brief 取 scene 段 */
    inline constexpr uint16 SceneOf(EntityID id)
    {
        return uint16((id >> kSceneShift) & kSceneMask);
    }

    /** @brief 取 index 段 */
    inline constexpr uint32 IndexOf(EntityID id)
    {
        return uint32((id >> kIndexShift) & kIndexMask);
    }

    /** @brief 取 version 段 */
    inline constexpr uint32 VersionOf(EntityID id)
    {
        return uint32(id & kVersionMask);
    }

} // namespace MMO::ECS
```

### 1.1 为什么不复用 EnTT 的 version（源码核实）

EnTT 3.16.0 的默认 `entt::entity` 是 **32 位**（`enum class entity : id_type{}`，`id_type = std::uint32_t`）。
其位分配写死在 `internal::entt_traits<std::uint32_t>`：

```cpp
using entity_type  = std::uint32_t;
using version_type = std::uint16_t;
static constexpr entity_type entity_mask  = 0xFFFFF;   // 低 20 位 = index
static constexpr entity_type version_mask = 0xFFF;     // 次 12 位 = version
```

**12 位 version 只有 4096 个回收代**。单场景 5w 实体、高频进出/刷怪回收下，同一 index 复用
4096 次即 version 回绕，旧句柄与新实体别名——AOI 差量、复制 diff、脚本延迟事件会静默指向错误实体。
20 位 index（`entity_mask=0xFFFFF`，104 万）也不足以覆盖 §3.1 要求的 2.68 亿槽位。

**结论（沿用 ECS_00 §3.1）**：我们自管 `index(28)+version(20)`，`entt::entity` 退化为
**scene 内部纯存储句柄，绝不外泄**。version 校验由 `EntityRegistry`（下节）在我们这一层做，
不依赖 EnTT 的 `to_version` / `valid`。EnTT 只负责组件 SoA 存储与视图遍历。

> 提示：`entt::to_integral(e)`（项目 `System.cpp` 已用）返回的是这个 32 位内部句柄的整数值，
> **不是** `EntityID`。P0 后所有对外句柄一律走 `EntityID`，`entt::entity` 不进任何跨层接口。

---

## 2. `EntityRegistry`——per-Scene 槽表 + free-list 回收

新建 `Src/Common/ECS/EntityRegistry.h`（target `CommonECS`，归属见 ECS_00 §6.3）。
它维护 `uint32 index → (entt::entity 句柄, uint32 version, bool alive)` 的槽表，
持有对 Scene 的 `entt::registry` 的引用（registry 由 Scene 拥有，见 §3）。

```cpp
/**
 * @file EntityRegistry.h
 * @brief per-Scene 实体槽表——uint64 EntityID ↔ entt::entity，index+version 自管回收
 *
 * 每个 Scene 一个 EntityRegistry。槽表按 index 稠密存储，销毁时 version++ 并回收 index，
 * 复用槽位时旧 EntityID 因 version 失配立即判失效（不依赖 EnTT 的 12 位 version）。
 */
#pragma once

#include <vector>

#include <entt/entt.hpp>

#include "Common/Core/MassiveAssert.h"
#include "Common/Core/Types.h"
#include "Common/ECS/EntityID.h"

namespace MMO::ECS
{

    /**
     * @brief 实体槽表——EntityID 的分配、解析、回收
     *
     * 只负责身份映射，不拥有 entt::registry（由 Scene 拥有并传入引用）。
     * 非线程安全——所有调用发生在 LogicThread 单线程内（见 ECS_00 §4 线程模型）。
     */
    class EntityRegistry
    {
    public:
        /**
         * @brief 构造
         * @param sceneID   本场景 ID（必须落在 16 位内）
         * @param registry  本场景的 entt::registry（Scene 拥有）
         */
        EntityRegistry(uint16 sceneID, entt::registry &registry)
            : _sceneID(sceneID), _registry(registry)
        {
        }

        EntityRegistry(const EntityRegistry &)            = delete;
        EntityRegistry &operator=(const EntityRegistry &) = delete;
        EntityRegistry(EntityRegistry &&)                 = delete;
        EntityRegistry &operator=(EntityRegistry &&)      = delete;

        ~EntityRegistry() = default;

        /**
         * @brief 分配一个新实体
         * @return 新 EntityID（version 取该槽当前代，全新槽为 0）
         */
        EntityID Create()
        {
            uint32 index = 0;
            if (!_freeList.empty())
            {
                index = _freeList.back();
                _freeList.pop_back();
            }
            else
            {
                index = static_cast<uint32>(_slots.size());
                MASSIVE_ASSERT(uint64(index) <= kIndexMask, "EntityRegistry: index 溢出 28 位");
                _slots.emplace_back();
            }

            Slot &slot  = _slots[index];
            slot.handle = _registry.create(); // EnTT 分配内部句柄
            slot.alive  = true;

            return MakeEntityID(_sceneID, index, slot.version);
        }

        /**
         * @brief 解析 EntityID → entt::entity（version 校验）
         * @return 有效则返回内部句柄；场景不符 / 越界 / 已销毁 / version 失配均返回 entt::null
         */
        entt::entity Resolve(EntityID id) const
        {
            if (SceneOf(id) != _sceneID)
            {
                return entt::null;
            }

            uint32 index = IndexOf(id);
            if (index >= _slots.size())
            {
                return entt::null;
            }

            const Slot &slot = _slots[index];
            if (!slot.alive || slot.version != VersionOf(id))
            {
                return entt::null;
            }

            return slot.handle;
        }

        /**
         * @brief 销毁实体——EnTT 侧删除 + version++ + 回收 index
         * @return true 已销毁；false 句柄无效（幂等，重复销毁安全）
         */
        bool Destroy(EntityID id)
        {
            entt::entity handle = Resolve(id);
            if (handle == entt::null)
            {
                return false;
            }

            _registry.destroy(handle); // 连带清理该实体所有组件

            Slot &slot   = _slots[IndexOf(id)];
            slot.handle  = entt::null;
            slot.alive   = false;
            slot.version = uint32((slot.version + 1) & kVersionMask); // 回绕安全（104 万代）
            _freeList.push_back(IndexOf(id));
            return true;
        }

        /** @brief 实体是否存活（version 校验） */
        bool IsAlive(EntityID id) const
        {
            return Resolve(id) != entt::null;
        }

        /** @brief 当前存活实体数（= 已分配槽 - 空闲槽） */
        size_t AliveCount() const
        {
            return _slots.size() - _freeList.size();
        }

    private:
        /** @brief 单个槽位 */
        struct Slot
        {
            entt::entity handle  = entt::null; // EnTT 内部句柄
            uint32       version = 0;          // 当前代，销毁时 ++
            bool         alive   = false;      // 是否存活
        };

        uint16                _sceneID;
        entt::registry       &_registry;
        std::vector<Slot>     _slots;    // 按 index 稠密
        std::vector<uint32>   _freeList; // 回收待复用的 index
    };

} // namespace MMO::ECS
```

### 2.1 用到的 EnTT 3.16.0 API（逐一核实）

| 调用 | 头文件签名（核实） | 用途 |
|---|---|---|
| `_registry.create()` | `[[nodiscard]] entity_type create()` | 分配内部句柄，存入 `slot.handle` |
| `_registry.destroy(handle)` | `version_type destroy(const entity_type entt)` | 删除实体连带全部组件 |
| `entt::null` | `inline constexpr null_t null{}`（可转任意 `Entity`、可与之比较） | 无效句柄哨兵 / 解析失败返回值 |

- **不使用** `_registry.valid()`：存活判定完全由我方 `slot.alive + version` 决定（`valid` 只反映 EnTT 12 位 version，见 §1.1）。
- **不使用** `entt::to_version` / `entt::to_entity`：EnTT 句柄的 index/version 语义与我方 28/20 布局无关，绝不混用。
- `entt::entity handle = entt::null;` 与 `handle == entt::null` 均由 `null_t` 的转换/比较运算符支持（`null_t` 比较只看 index 段，用于哨兵判定足够）。

### 2.2 回收语义与并发约束

- **version 单调递增、回绕安全**：`(slot.version + 1) & kVersionMask`，同槽复用 104 万次才回绕；回绕后极端别名概率可忽略，且远高于 EnTT 的 4096 代。
- **`Destroy` 幂等**：内部先 `Resolve`，失配即 `return false`，重复销毁 / 陈旧句柄销毁都安全。
- **单线程**：所有分配/销毁/解析发生在 LogicThread 阶段 1~7 内（ECS_00 §4），故槽表无锁。阶段 8 复制只读快照期不得调用 `Create`/`Destroy`（ECS_05 保证）。

---

## 3. 重写 `Scene.h` / `Scene.cpp`

改造点：删除 `RegisterScriptComponent` 声明与所有 `ScriptComponentStorage` 文档引用；
`Scene` 拥有 `entt::registry` + `EntityRegistry`；对外接口全部改用 `EntityID`；保留删拷贝/删移动惯用法。

### 3.1 `Src/Common/ECS/Scene.h`（重写后全文）

```cpp
/**
 * @file Scene.h
 * @brief 场景——entt::registry（组件唯一所有者）+ EntityRegistry（uint64 身份）容器
 *
 * 每个场景独立一个 Scene 实例，持有独立 EnTT registry。
 * C++ 高频组件（Position/Velocity/Health/…）走 EnTT registry 的 SoA 存储，
 * 组件数据永远只活在 C++/EnTT（ECS_00 铁律 1）。对外句柄一律 uint64 EntityID。
 */
#pragma once

#include <entt/entt.hpp>

#include "Common/Core/MassiveAssert.h"
#include "Common/Core/Types.h"
#include "Common/ECS/EntityID.h"
#include "Common/ECS/EntityRegistry.h"

namespace MMO::ECS
{

    /**
     * @brief 场景——entt::registry + EntityRegistry 的容器
     *
     * registry 是场景内全部组件的唯一所有者；EntityRegistry 负责 uint64 ↔ entt::entity
     * 映射与 version 回收。二者一一绑定，随 Scene 一同构造/析构。
     */
    class Scene
    {
    public:
        explicit Scene(uint32 sceneID)
            : _sceneID(sceneID), _entities(ToSceneU16(sceneID), _registry)
        {
        }

        Scene(const Scene &)            = delete;
        Scene &operator=(const Scene &) = delete;
        Scene(Scene &&)                 = delete;
        Scene &operator=(Scene &&)      = delete;

        ~Scene() = default;

        uint32 SceneID() const
        {
            return _sceneID;
        }

        // ── Entity 生命周期 ──

        /**
         * @brief 创建实体
         * @return 新实体 EntityID
         */
        EntityID CreateEntity()
        {
            return _entities.Create();
        }

        /**
         * @brief 销毁实体（连带清理其全部组件）
         * @return true 已销毁；false 句柄失效
         */
        bool DestroyEntity(EntityID id)
        {
            return _entities.Destroy(id);
        }

        /** @brief 实体是否存活（version 校验） */
        bool IsValid(EntityID id) const
        {
            return _entities.IsAlive(id);
        }

        /**
         * @brief EntityID → entt::entity（内部/系统热循环用；返回 entt::null 表示失效）
         * @warning 返回值仅限本 tick 本场景内部使用，绝不外泄
         */
        entt::entity Resolve(EntityID id) const
        {
            return _entities.Resolve(id);
        }

        // ── C++ 组件（EnTT 存储，入参一律 EntityID）──

        template <typename T>
        T &GetComponent(EntityID id)
        {
            entt::entity e = _entities.Resolve(id);
            MASSIVE_ASSERT(e != entt::null, "GetComponent: 实体已失效");
            return _registry.get<T>(e);
        }

        template <typename T>
        const T &GetComponent(EntityID id) const
        {
            entt::entity e = _entities.Resolve(id);
            MASSIVE_ASSERT(e != entt::null, "GetComponent: 实体已失效");
            return _registry.get<T>(e);
        }

        template <typename T>
        bool HasComponent(EntityID id) const
        {
            entt::entity e = _entities.Resolve(id);
            if (e == entt::null)
            {
                return false;
            }
            return _registry.all_of<T>(e);
        }

        template <typename T, typename... Args>
        T &EmplaceComponent(EntityID id, Args &&...args)
        {
            entt::entity e = _entities.Resolve(id);
            MASSIVE_ASSERT(e != entt::null, "EmplaceComponent: 实体已失效");
            return _registry.emplace<T>(e, std::forward<Args>(args)...);
        }

        template <typename T>
        void RemoveComponent(EntityID id)
        {
            entt::entity e = _entities.Resolve(id);
            if (e == entt::null)
            {
                return;
            }
            _registry.remove<T>(e); // 安全：组件不存在时返回 0，不报错
        }

        // ── 直达 registry（系统热循环走视图/组，不逐个 Resolve）──

        entt::registry &Registry()
        {
            return _registry;
        }

        const entt::registry &Registry() const
        {
            return _registry;
        }

        EntityRegistry &Entities()
        {
            return _entities;
        }

    private:
        /** @brief sceneID 收窄到 16 位（scene 段容量），越界即断言 */
        static uint16 ToSceneU16(uint32 sceneID)
        {
            MASSIVE_ASSERT(uint64(sceneID) <= kSceneMask && sceneID != 0,
                           "Scene: sceneID 必须落在 [1, 65535]");
            return static_cast<uint16>(sceneID);
        }

        uint32         _sceneID;
        entt::registry _registry; // 组件唯一所有者（先于 _entities 声明，确保引用有效）
        EntityRegistry _entities; // 引用 _registry，故必须后声明
    };

} // namespace MMO::ECS
```

> **成员声明顺序要点**：`_registry` 必须先于 `_entities` 声明——`EntityRegistry` 构造时持有
> `_registry` 的引用，C++ 成员按声明序初始化，顺序颠倒即引用未构造成员（UB）。

### 3.2 `Src/Common/ECS/Scene.cpp`

`CreateEntity` / `DestroyEntity` / `IsValid` 已全部内联到头文件（转发给 `EntityRegistry`），
`.cpp` 里原有的 `Scene()` / `CreateEntity` / `DestroyEntity` / `IsValid` 定义全部删除，Scene 不再有任何非模板、非内联成员。

**但不能直接删掉 `Scene.cpp` 文件。** 真实 `Src/Common/ECS/xmake.lua` 是：

```lua
target("CommonECS")
    set_kind("static")
    add_files("*.cpp")   -- 通配符，非显式列表
    add_headerfiles("*.h")
    add_deps("CommonCore", "CommonLog")
    add_deps("entt", {public = true})
    add_deps("libDaScript", {public = true})
```

`Scene.cpp` 是该目录唯一的 `.cpp`。若删文件，`add_files("*.cpp")` 匹配零文件 → `static` 库无对象文件（xmake 报错或产出空 archive，行为随版本而定）。因此**二选一**（本篇推荐方案 A）：

- **方案 A（推荐，改动最小）**：保留 `Scene.cpp`，内容清空为一个只含文件头 + `#include "Common/ECS/Scene.h"` 的空 TU。`add_files("*.cpp")` 仍匹配到它，`static` 目标有对象文件，xmake 不变。
- **方案 B**：把 `CommonECS` 改为 header-only（项目已有先例 `Src/Common/Queue/xmake.lua`：`set_kind("headeronly")` + `add_headerfiles("*.h")`，去掉 `add_files`），然后删 `Scene.cpp`。注意此时 `EntityRegistry.h` 等也全为 header-only，且下游 `add_deps("CommonECS")` 对 headeronly 目标只吃到 include 路径 —— 需确认 `CommonECS` 没有其他必须编译的 TU（当前没有）。

### 3.3 相对现状源码的删除项

- 删 `void RegisterScriptComponent(const std::string &name, size_t componentSize);`（现 `Scene.h:206`）——
  无定义、无调用，是删脚本层 ECS（commit `1d3f1a4a`）遗留的链接期地雷（ECS_00 §0 已核实）。
- 删类头 doxygen 与文件头里所有 “ScriptComponentStorage / 脚本组件 SoA Blob 列” 描述——该存储已不存在，
  且违反铁律 1（组件数据只活在 EnTT）。
- 删旧 `CreateView` / `CreateGroup` 模板：现有实现 `_registry.group<entt::owned_t<Owned...>>()`
  的参数形态与 EnTT 3.16.0 的 `group<Owned...>(get_t<...>, exclude_t<...>)` 不匹配（易误用）。
  系统需要视图/组时直接用 `scene.Registry().view<...>()` / `.group<...>()`（`System.cpp` 已是此写法），
  不再经 Scene 转发。

---

## 4. 删除 `DirtyTracker.h`

**明确动作：删除 `Src/Common/ECS/DirtyTracker.h`。**

- 源码核实：`DirtyTracker` 在 `Src/` 全树 **零引用**——无 `#include`、无实例化、无调用点（ECS_00 §0 / current-ecs 事实）。
- 它按 `uint32 entityID` 存脏集，与新的 `uint64 EntityID` 语义不符，且 per-component `unordered_set`
  在 5w 实体规模下不是复制 diff 的正解。
- 复制脏标记由 `ECS_02` 的 **`DirtyIndex`**（bitset / 稠密位图，按组件类型）承担，dirty-driven 复制在 `ECS_05`。
- 删除后无任何 TU 受影响（零引用）；xmake 文件列表若显式列了它，一并移除（见 §7）。

---

## 5. 玩家 identity 归位

现状：玩家身份散在 `WorldSession::entity`（跨表 `struct Entity`），实体本身不知道自己属于哪个 session。
P0 把身份**归位到组件**：实体上带 `PlayerConn{sessionID}`（entity → session 反查），
`WorldSession` 只留 `entityID`（session → entity 正查）。二者构成完整双向映射，身份数据落在 EnTT。

### 5.1 新增组件 `PlayerConn`

新建 `Src/World/Component/PlayerConn.h`（World 专用玩法数据，归属见 ECS_00 §2 表）：

```cpp
/**
 * @file PlayerConn.h
 * @brief 玩家连接 Component——把 sessionID 挂到玩家实体上（entity → session 反查）
 *
 * 仅玩家实体拥有。C++ 复制/事件派发从实体反查所属 WorldSession 时用它，
 * 避免遍历 _sessions。session → entity 正查走 WorldSession::entityID。
 */
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 玩家连接标识
     *
     * sessionID 为 Gate 分配的会话 ID（0 视为未绑定）。
     */
    struct PlayerConn
    {
        uint32 sessionID = 0;
    };

} // namespace MMO
```

> `PlayerTag`（已存在于 `Component/Tags.h`）标记“这是玩家”，`PlayerConn` 携带“属于哪个 session”。
> 二者分离：tag 用于视图筛选（零存储），`PlayerConn` 携数据。`Position` / `Health` 见 ECS_02 组件集。

### 5.2 `WorldSession.h` 字段改动

把跨表 `Entity entity;` 换成 `EntityID entityID;`，头文件 include 随之更换。

改动前（现状 `WorldSession.h:13, 33`）：

```cpp
#include "Common/ECS/Entity.h"
// ...
        Entity        entity;           // World 侧的玩家 Entity
```

改动后：

```cpp
#include "Common/ECS/EntityID.h"
// ...
        ECS::EntityID entityID = ECS::kInvalidEntityID; // World 侧玩家实体（session → entity 正查）
```

> `WorldSession::entityID` 本身**就是 sessionID → EntityID 的查找**：`_sessions[sessionID].entityID`。
> 无需额外的 map。反向（entity → session）由 `PlayerConn` 组件承担。
> `Src/Common/ECS/Entity.h`（旧 `struct Entity` + `std::hash` 特化）在所有引用迁移后删除（见 §7）。

### 5.3 `EnterWorldHandler` 改动

`HandleFirstLogin` / `HandleReconnect` / `SendRsp` 里的 `entity` 用法迁移到 `entityID`，
并在首次登录时直接 emplace 玩家的初始组件（P0 无 prefab，直接 emplace；prefab 见 §6 / ECS_08）。

`HandleFirstLogin`（现状 `EnterWorldHandler.cpp:96-128`）改为：

```cpp
    void EnterWorldHandler::HandleFirstLogin(uint32                                    sessionID,
                                             uint16                                    gateServerID,
                                             std::unordered_map<uint32, WorldSession> &sessions,
                                             uint32                                    accountID,
                                             const uint8                              *sessionKey,
                                             uint64                                    clientRandom,
                                             ECS::Scene                               &defaultScene,
                                             GateSendFn                                gateSendFn)
    {
        // 1. 创建实体 + 直接 emplace 初始组件（P0 无 prefab；组件类型见 ECS_02）
        ECS::EntityID entityID = defaultScene.CreateEntity();
        defaultScene.EmplaceComponent<PlayerConn>(entityID, PlayerConn {sessionID});
        defaultScene.EmplaceComponent<PlayerTag>(entityID);
        defaultScene.EmplaceComponent<Position>(entityID, Position {0.0f, 0.0f, 0.0f});
        defaultScene.EmplaceComponent<Health>(entityID, Health {100, 100});

        // 2. 加密上下文
        CryptoSession crypto;
        crypto.Init(sessionKey, clientRandom);

        // 3. 建会话
        WorldSession ws;
        ws.sessionID    = sessionID;
        ws.accountID    = accountID;
        ws.entityID     = entityID;
        ws.crypto       = std::move(crypto);
        ws.gateServerID = gateServerID;
        ws.lastRecvTime = std::chrono::steady_clock::now();
        ws.disconnected = false;

        sessions[sessionID] = std::move(ws);

        Log::Info("EnterWorld: accountID={} → entityID={:#x} sessionID={}",
                  accountID,
                  entityID,
                  sessionID);

        // 4. 回包（P0 兼容旧 uint32 wire：player_id=index 段，scene_id=scene 段）
        SendRsp(sessionID,
                ECS::IndexOf(entityID),
                ECS::SceneOf(entityID),
                0.0f, 0.0f, 0.0f,
                gateSendFn);
    }
```

`HandleReconnect` 末尾的回包（现状 `EnterWorldHandler.cpp:187`）同样改用 `entityID` 拆段：

```cpp
        SendRsp(sessionID,
                ECS::IndexOf(ws.entityID),
                ECS::SceneOf(ws.entityID),
                0.0f, 0.0f, 0.0f,
                gateSendFn);
```

`EnterWorldHandler.cpp` 顶部新增 include：

```cpp
#include "World/Component/Health.h"
#include "World/Component/PlayerConn.h"
#include "World/Component/Position.h"
#include "World/Component/Tags.h"
```

`EnterWorldHandler.h` 同步删除对旧 `Entity` 的 include（现状 `EnterWorldHandler.h:13` 逐字为
`#include "Common/ECS/Entity.h"`）——该头已不再引用 `Entity` 类型（`ws.entity` 已改为 `ws.entityID`，
`EntityID` 通过 `WorldSession.h` → `Common/ECS/EntityID.h` 传递）。若 `EnterWorldHandler.h` 的函数签名
出现过 `Entity` 参数/返回，一并改为 `ECS::EntityID`。**这一步必须在删 `Entity.h`（§7）之前做**，否则删头后
`EnterWorldHandler.h` 编译失败。

> **强制偏差（wire 协议，需 orchestrator 知悉）**：`Proto::LoginEnterWorldRsp` 的
> `player_id` / `scene_id` 仍是 **uint32**（`Login.proto:50-51`），`SendRsp` 签名也是 uint32。
> P0 **不改 proto**——回包用 `IndexOf(entityID)`（scene 内句柄）+ `SceneOf(entityID)` 拆段填入，
> 保持客户端契约不变。完整 uint64 `EntityID` 上线（proto 增 `uint64` 字段 / 复制 diff 带全 id）
> 属于 `ECS_05` 复制系统职责，本篇不做。

### 5.4 `WorldServer.cpp` 调用点迁移（现状已引用 `ws.entity`）

现状 `WorldServer.cpp` 有 3 处引用旧 `Entity`，随字段改名一并迁移（均为机械替换）：

| 位置（现状行） | 改动前 | 改动后 |
|---|---|---|
| `:463` | `_sceneMgr.GetScene(it->second.entity.sceneId)` | `_sceneMgr.GetScene(ECS::SceneOf(it->second.entityID))` |
| `:466` | `scene->DestroyEntity(it->second.entity)` | `scene->DestroyEntity(it->second.entityID)` |
| `:646` | `ws.disconnected \|\| !ws.entity.IsValid()` | `ws.disconnected \|\| ws.entityID == ECS::kInvalidEntityID` |
| `:651` | `uint32 playerEID = ws.entity.entityId;` | `ECS::EntityID playerEID = ws.entityID;` |

> `:651` 及其后的 `SystemReplicate` 逻辑用 `playerEID` 与 `visibleSets`（`unordered_map<uint32,...>`）
> 交互——该键类型（uint32 vs EntityID）的统一属复制系统重构，落在 `ECS_05`；P0 只做字段改名与
> `GetScene`/`DestroyEntity` 的 EntityID 化，`SystemReplicate` 现状本就零调用（ECS_00 §0），不阻塞 P0 编译。

---

## 6. Prefab 组合层——接口（实现见 ECS_08）

依 ECS_00 §6.5：prefab 配方用 **daslang 表**写（玩法作者掌控、可热重载），C++ 侧 `Bridge_Spawn`
查表并 emplace 组件（emplace 始终 C++ 做，保证组件类型固定 + 热重载安全，铁律 1）。
**本篇只定接口与表形态，完整查表/emplace 实现落在 `ECS_08`。**

### 6.1 C++ 接口（extern 声明）

新建 `Src/World/DasModule/WorldBridge.h`（`WorldBridgeModule` 的 Bridge_* 面，归属见 ECS_00 §2 表）：

```cpp
/**
 * @file WorldBridge.h
 * @brief 脚本 ↔ C++ 实体桥接面（Bridge_*）声明——供 WorldBridgeModule 绑定给 daslang
 *
 * 所有 Bridge_* 只收发纯整数 / 标量（EntityID/sceneID/坐标），绝不传组件对象或 entt 句柄（铁律 1）。
 * 完整实现见 ECS_06（绑定机制）/ ECS_08（prefab 查表落地）。
 */
#pragma once

#include "Common/Core/Types.h"
#include "Common/ECS/EntityID.h"

namespace MMO
{

    /**
     * @brief 按 prefab 配方在指定场景生成一个实体
     *
     * C++ 查 daslang 注册的 prefab 配方表（§6.2），按配方直接 emplace 各组件，
     * 设置初始 Position，返回新实体 EntityID。查不到配方 / 场景不存在返回 kInvalidEntityID。
     *
     * @param sceneID     目标场景 ID（16 位）
     * @param prefabName  prefab 名（对应配方表 key）
     * @param x,y,z       初始世界坐标
     * @return 新实体 EntityID；失败 kInvalidEntityID
     */
    ECS::EntityID Bridge_Spawn(uint16 sceneID, const char *prefabName, float x, float y, float z);

} // namespace MMO
```

绑定给 daslang 的方式与现有 `DasCommonModule::Build()` 中的 `LogInfo` 完全同构（daslang-binding 事实核实）：

```cpp
    das::addExtern<DAS_BIND_FUN(MMO::Bridge_Spawn)>(*this,
                                                    lib,
                                                    "Bridge_Spawn",
                                                    das::SideEffects::modifyExternal,
                                                    "MMO::Bridge_Spawn")
        ->args({"sceneID", "prefabName", "x", "y", "z"});
```

脚本侧调用形态（`SideEffects::modifyExternal` 因其改动 C++ 世界状态）：

```das
let id = Bridge_Spawn(uint16(1), "goblin", 10.0, 0.0, 5.0)
```

### 6.2 daslang 配方表形态

新建 `Script/World/Prefabs.das`（玩法作者维护、随脚本热重载）：

```das
options gen2
options indenting = 4

// prefab 配方：一个 prefab 的组件初值。字段对应 C++ 组件（ECS_02 组件集）。
struct PrefabDef
    maxHp     : int
    moveSpeed : int
    isMonster : bool

// 配方表：prefabName -> PrefabDef。C++ Bridge_Spawn 按 name 查此表后 emplace。
var g_prefabs : table<string; PrefabDef>

[export]
def RegisterPrefabs
    g_prefabs["goblin"]  <- PrefabDef(maxHp = 30,  moveSpeed = 120, isMonster = true)
    g_prefabs["knight"]  <- PrefabDef(maxHp = 200, moveSpeed = 100, isMonster = true)
```

**C++ 如何查这张表（接口约定，实现见 ECS_08）**：`WorldBridgeModule` 在 `OnContextSwapped`
（`IDasLangModuleProvider` 接口，脚本层已定）拿到新 `das::Context` 后，把 das 侧 `g_prefabs`
（或经一个 `[export] def LookupPrefab(name; var out)` 访问器）读入 C++ 侧的
`unordered_map<string, PrefabDef>` 快照；`Bridge_Spawn` 命中后按 `PrefabDef` 字段
`EmplaceComponent<Health>` / `<Velocity>` / `<MonsterTag>` 等。热重载时随新 context 重建快照——
组件数据始终在 EnTT，配方数据随 das context 销毁重建，不违反铁律 1。**完整读取与 emplace 代码见 `ECS_08`。**

---

## 7. 迁移步骤清单（可执行）

按序执行，每步后可编译。验证方法：`xmake build CommonECS && xmake build WorldServer`（P0 无脚本改动，
`Prefabs.das` 仅新增文件，用 `xmake run <daslang ast 检查 target>` 或 `python -c "..."` 对齐脚本层已有的
`ast.parse` 校验；prefab 查表实现在 ECS_08 才接线，本篇 `Prefabs.das` 只需语法通过）。

- [ ] **新增** `Src/Common/ECS/EntityID.h`（§1，逐字照抄 ECS_00 §3.1）。
- [ ] **新增** `Src/Common/ECS/EntityRegistry.h`（§2）。
- [ ] **重写** `Src/Common/ECS/Scene.h`（§3.1）：拥有 `entt::registry` + `EntityRegistry`；接口改 `EntityID`；
      删 `RegisterScriptComponent` 声明、删 ScriptComponentStorage 文档、删 `CreateView`/`CreateGroup`；
      保留删拷贝/删移动。
- [ ] **清空** `Src/Common/ECS/Scene.cpp`（§3.2 方案 A，成员已内联）：改为只含文件头 +
      `#include "Common/ECS/Scene.h"` 的空 TU。**不要删文件**——`CommonECS` 是 `set_kind("static")` +
      `add_files("*.cpp")` 通配，`Scene.cpp` 是唯一 `.cpp`，删掉会导致 static 目标零对象文件。
      （或按方案 B 改 `CommonECS` 为 `headeronly` 后再删，见 §3.2。）
- [ ] **删除** `Src/Common/ECS/DirtyTracker.h`（§4，零引用死代码）。
- [ ] **新增** `Src/World/Component/PlayerConn.h`（§5.1）。
- [ ] **改** `Src/World/WorldSession.h`（§5.2）：`Entity entity` → `ECS::EntityID entityID`；
      include `Entity.h` → `EntityID.h`。
- [ ] **改** `Src/World/Handler/EnterWorldHandler.cpp`（§5.3）：`HandleFirstLogin` 建实体 + emplace
      `PlayerConn`/`PlayerTag`/`Position`/`Health`；`HandleFirstLogin`/`HandleReconnect` 回包用
      `IndexOf`/`SceneOf` 拆段；补组件 include。
- [ ] **改** `Src/World/WorldServer.cpp`（§5.4）：4 处 `ws.entity` 调用点机械迁移到 `entityID`。
- [ ] **删除** `Src/Common/ECS/Entity.h`（旧 `struct Entity` + `std::hash`）——确认 grep 全树无残余引用后删；
      同步移除 `EnterWorldHandler.h` 等处的 `#include "Common/ECS/Entity.h"`。
- [ ] **新增** `Src/World/DasModule/WorldBridge.h`（§6.1，仅 `Bridge_Spawn` 声明；实现体 ECS_08）。
- [ ] **新增** `Script/World/Prefabs.das`（§6.2，配方表 + `RegisterPrefabs`）。
- [ ] **验证**：`xmake build CommonECS` 通过（新身份层独立可编译）；`xmake build WorldServer` 通过
      （EnterWorld/WorldServer 迁移不留悬空引用）；`Prefabs.das` 语法/`ast` 校验通过。
      全树 `grep "RegisterScriptComponent\|DirtyTracker\|struct Entity\b"` 应为空（除本篇/ECS_00 文档）。

> **不属于 P0（明确后延）**：`Bridge_Spawn` 查表 emplace 实现（`ECS_08`）；proto 增 uint64 实体字段与
> 复制 diff 带全 id（`ECS_05`）；`DirtyIndex` 脏标记（`ECS_02`）；`SystemReplicate` 的 `visibleSets`
> 键从 uint32 统一到 EntityID（`ECS_05`）。本篇给出的均为 P0 完整可编译代码，无占位。
