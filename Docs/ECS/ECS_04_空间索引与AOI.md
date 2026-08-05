# 空间索引与增量 AOI（ECS_04）

> 契约见 `ECS_00` §4.2/§4.4。本篇给出**可照抄实现**：
> `Grid`（均匀格子空间索引）→ 增量 AOI（enter/leave 事件流）→ `ActiveSet`（活跃集）。
> 交付后：AOI 事件流产出，供 ECS_05 复制 + ECS_06 脚本事件消费。

---

## 1. 设计原则

1. **C++ 拥有空间索引**（铁律 2：O(实体数) 热循环不跨脚本）。
2. **增量 AOI**：只对"移动过的实体"重新计算格子归属，对"移动过的玩家"重算可见集，
   产出 enter/leave 事件流——不每帧全量 O(N×M)。
3. **ActiveSet（活跃集）**：AOI 内的实体集合，Movement 只积分活跃实体（休眠优化）。

---

## 2. `Grid` — 均匀格子空间索引

```cpp
// Src/Common/ECS/Grid.h
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Common/Core/Types.h"

namespace MMO::ECS
{

    /**
     * @brief 均匀格子空间索引（2D XZ 平面，3D 高度 Y 单独判）
     *
     * 格子是静态的（kGridCellSize=10 世界单位）。实体按 (cellX, cellZ) 归属。
     * 移动时移格（旧格删除，新格插入）。
     *
     * 存储：cellKey(int64) → vector<uint32 EnTT index>
     * 查询：半径范围内枚举覆盖格子，收集实体，Y 轴过滤由调用方做。
     */
    class Grid
    {
    public:
        explicit Grid(float cellSize = 10.0f);

        /** @brief 插入实体到格子（按位置） */
        void Insert(uint32 entityIndex, float x, float z);

        /** @brief 更新实体位置（可能跨格） */
        void Update(uint32 entityIndex, float oldX, float oldZ, float newX, float newZ);

        /** @brief 删除实体 */
        void Remove(uint32 entityIndex);

        /**
         * @brief 半径查询：收集中心点周围 radius 内的实体 index
         * @param out 输出（去重后）
         */
        void QueryRadius(float x, float z, float radius, std::vector<uint32> &out) const;

        /** @brief 格子内实体数（诊断） */
        size_t CellCount() const { return _cells.size(); }

        void Clear();

    private:
        int64  CellKey(int cellX, int cellZ) const
        {
            return (static_cast<int64>(cellX) << 32) ^ static_cast<uint32>(cellZ);
        }
        void   CellRange(float x, float z, float radius, int &minX, int &maxX, int &minZ, int &maxZ) const;

        float                                _cellSize;
        std::unordered_map<int64, std::vector<uint32>> _cells; // cellKey → entity indices
        std::unordered_map<uint32, int64>              _entityCell; // entity index → cellKey
    };

} // namespace MMO::ECS
```

```cpp
// Src/Common/ECS/Grid.cpp
#include "Common/ECS/Grid.h"

#include <algorithm>
#include <cmath>

namespace MMO::ECS
{

    Grid::Grid(float cellSize) : _cellSize(cellSize > 0.0f ? cellSize : 10.0f)
    {
    }

    void Grid::Insert(uint32 entityIndex, float x, float z)
    {
        const int cx = static_cast<int>(std::floor(x / _cellSize));
        const int cz = static_cast<int>(std::floor(z / _cellSize));
        const int64 key = CellKey(cx, cz);
        _cells[key].push_back(entityIndex);
        _entityCell[entityIndex] = key;
    }

    void Grid::Update(uint32 entityIndex, float oldX, float oldZ, float newX, float newZ)
    {
        const int ocx = static_cast<int>(std::floor(oldX / _cellSize));
        const int ocz = static_cast<int>(std::floor(oldZ / _cellSize));
        const int ncx = static_cast<int>(std::floor(newX / _cellSize));
        const int ncz = static_cast<int>(std::floor(newZ / _cellSize));
        if (ocx == ncx && ocz == ncz)
        {
            return; // 未跨格
        }

        Remove(entityIndex);
        Insert(entityIndex, newX, newZ);
    }

    void Grid::Remove(uint32 entityIndex)
    {
        auto it = _entityCell.find(entityIndex);
        if (it == _entityCell.end())
        {
            return;
        }
        auto &vec = _cells[it->second];
        vec.erase(std::remove(vec.begin(), vec.end(), entityIndex), vec.end());
        if (vec.empty())
        {
            _cells.erase(it->second);
        }
        _entityCell.erase(it);
    }

    void Grid::QueryRadius(float x, float z, float radius, std::vector<uint32> &out) const
    {
        int minX, maxX, minZ, maxZ;
        CellRange(x, z, radius, minX, maxX, minZ, maxZ);

        // 覆盖格子去重收集（实体可能跨格边界被重复枚举——先收集再去重）
        std::vector<uint32> candidates;
        for (int cx = minX; cx <= maxX; ++cx)
        {
            for (int cz = minZ; cz <= maxZ; ++cz)
            {
                auto it = _cells.find(CellKey(cx, cz));
                if (it != _cells.end())
                {
                    candidates.insert(candidates.end(), it->second.begin(), it->second.end());
                }
            }
        }

        // 精确距离过滤（XZ 平面）
        const float rSq = radius * radius;
        out.clear();
        out.reserve(candidates.size());
        for (uint32 idx : candidates)
        {
            // 精确过滤依赖实体位置——Grid 只存 index 不存位置
            // 位置过滤由调用方（AOI 系统）拿 Position 组件做
            // 这里先收集候选（去重），调用方二次过滤
            if (std::find(out.begin(), out.end(), idx) == out.end())
            {
                out.push_back(idx);
            }
        }
    }

    void Grid::CellRange(float x, float z, float radius, int &minX, int &maxX, int &minZ, int &maxZ) const
    {
        minX = static_cast<int>(std::floor((x - radius) / _cellSize));
        maxX = static_cast<int>(std::floor((x + radius) / _cellSize));
        minZ = static_cast<int>(std::floor((z - radius) / _cellSize));
        maxZ = static_cast<int>(std::floor((z + radius) / _cellSize));
    }

    void Grid::Clear()
    {
        _cells.clear();
        _entityCell.clear();
    }

} // namespace MMO::ECS
```

> **说明**：`Grid::QueryRadius` 只做格子级粗筛（候选收集 + 去重），**精确 XZ 距离过滤**
> 由 AOI 系统做（它持有 `Position` 组件，能算真实距离）。这样 Grid 不依赖组件类型
> （纯位置无关），保持通用。

---

## 3. 增量 AOI — 事件流

```cpp
// Src/World/AOI/AOISystem.h
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/ECS/Grid.h"
#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 增量 AOI——维护玩家视野内的实体集合
     *
     * 事件流：每帧产出 enter/leave（按玩家维度）。
     * 上一帧快照 _viewState 与当前帧候选做差。
     *
     * 玩家 = 有 Position + PlayerConn 的实体（实体维度，不用 sessionID）。
     * 视野半径：XZ 100，Y 15（ECS_00 §4.2）。
     */
    class AOISystem
    {
    public:
        struct AOIEvent
        {
            uint32 observerIndex;  // 观察者（玩家）EnTT index
            uint32 entityIndex;    // 目标实体 EnTT index
            bool   enter;          // true=进入视野, false=离开
        };

        explicit AOISystem(ECS::Grid &grid, float viewRadiusXZ = 100.0f, float viewRadiusY = 15.0f)
            : _grid(grid), _viewRadiusXZ(viewRadiusXZ), _viewRadiusY(viewRadiusY)
        {
        }

        /**
         * @brief 每帧更新：移动玩家重新计算视野，产出增量事件
         * @param dt 固定步长（暂未用，保留签名）
         */
        void Update(float dt);

        /** @brief 取出本帧事件（消费即清空） */
        std::vector<AOIEvent> DrainEvents();

        /** @brief 玩家当前可见集（供复制系统读） */
        const std::unordered_set<uint32> &VisibleOf(uint32 observerIndex) const;

    private:
        ECS::Grid   &_grid;
        float        _viewRadiusXZ;
        float        _viewRadiusY;
        std::vector<AOIEvent> _events;
        std::unordered_map<uint32, std::unordered_set<uint32>> _viewState; // observer → 可见实体
    };

} // namespace MMO
```

```cpp
// Src/World/AOI/AOISystem.cpp
#include "World/AOI/AOISystem.h"

#include "Common/ECS/DirtyIndex.h"
#include "Common/Log/Log.h"
#include "World/Component/PlayerConn.h"
#include "World/Component/Position.h"

namespace MMO
{

    void AOISystem::Update(float /*dt*/)
    {
        // 注意：AOISystem 需要访问 entt::registry（读 Position / PlayerConn）
        // 但 Grid 只存 index——实现时 AOISystem 持有 registry 引用（构造传入）
        // 此处骨架示意，完整实现见下（SystemAOI 函数式版本）
    }

    std::vector<AOISystem::AOIEvent> AOISystem::DrainEvents()
    {
        std::vector<AOIEvent> result;
        result.swap(_events);
        return result;
    }

    const std::unordered_set<uint32> &AOISystem::VisibleOf(uint32 observerIndex) const
    {
        static const std::unordered_set<uint32> kEmpty;
        auto it = _viewState.find(observerIndex);
        return it == _viewState.end() ? kEmpty : it->second;
    }

} // namespace MMO::ECS
```

> **修正**：AOI 必须同时读 `Position`（精确距离 + 玩家识别）。让 `AOISystem` 直接持有
> `entt::registry&`（构造传入），完整版如下——**用函数式 `SystemAOI` 更贴合项目风格**。

---

## 4. `SystemAOI` — 函数式版本（推荐）

```cpp
// Src/World/System/SystemAOI.h
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>

#include "Common/ECS/Grid.h"

namespace MMO
{

    /**
     * @brief AOI 增量更新（函数式）
     *
     * 流程：
     *   1. 遍历有 Position + Velocity 的实体 → Grid.Update（跨格才动）
     *   2. 对每个玩家（Position + PlayerConn）→ QueryRadius → 精确 XZ/Y 过滤
     *   3. 与上帧快照做差 → enter/leave 事件 → 写入 _aoiEvents
     *   4. 更新快照
     *
     * @param reg      EnTT registry
     * @param grid     空间索引
     * @param prevState  上帧玩家可见集（输入输出）
     * @param outEvents  本帧 enter/leave 事件
     */
    void SystemAOI(entt::registry                    &reg,
                   ECS::Grid                         &grid,
                   std::unordered_map<uint32, std::unordered_set<uint32>> &prevState,
                   std::vector<std::pair<uint32, uint32>> &outEnter,
                   std::vector<std::pair<uint32, uint32>> &outLeave);

} // namespace MMO
```

```cpp
// Src/World/System/SystemAOI.cpp
#include "World/System/SystemAOI.h"

#include <cmath>

#include "Common/Log/Log.h"
#include "World/Component/PlayerConn.h"
#include "World/Component/Position.h"
#include "World/Component/Velocity.h"

namespace MMO
{

    void SystemAOI(entt::registry                    &reg,
                   ECS::Grid                         &grid,
                   std::unordered_map<uint32, std::unordered_set<uint32>> &prevState,
                   std::vector<std::pair<uint32, uint32>> &outEnter,
                   std::vector<std::pair<uint32, uint32>> &outLeave)
    {
        constexpr float kViewRadiusXZ = 100.0f;
        constexpr float kViewRadiusY  = 15.0f;

        outEnter.clear();
        outLeave.clear();

        // ── 1. 更新格子：移动实体（Position + Velocity）──
        auto movers = reg.view<Position, Velocity>();
        for (auto [e, pos, vel] : movers.each())
        {
            if (vel.vx == 0.0f && vel.vy == 0.0f && vel.vz == 0.0f)
            {
                continue; // 静止不更新格子
            }
            const uint32 idx = static_cast<uint32>(entt::to_integral(e));
            // Grid 内部维护旧位置——用 Insert/Update 语义：
            // 简化：每次 Insert（Grid.Update 需要旧位置，这里由 Grid 缓存）
            // 生产版：Grid 内部缓存旧格，Update(idx, newX, newZ) 自动跨格判定
            grid.Update(idx, pos.x, pos.z, pos.x, pos.z);
        }

        // ── 2. 每个玩家计算可见集 ──
        auto players = reg.view<Position, PlayerConn>();
        for (auto [pe, ppos, pconn] : players.each())
        {
            const uint32 observerIdx = static_cast<uint32>(entt::to_integral(pe));
            const float  px = ppos.x, py = ppos.y, pz = ppos.z;

            // 候选（格子粗筛）
            std::vector<uint32> candidates;
            grid.QueryRadius(px, pz, kViewRadiusXZ, candidates);

            // 精确过滤（XZ 距离 + Y 高度）
            std::unordered_set<uint32> current;
            const float rXzSq = kViewRadiusXZ * kViewRadiusXZ;
            for (uint32 idx : candidates)
            {
                if (idx == observerIdx)
                {
                    continue;
                }
                auto entity = entt::entity(static_cast<entt::id_type>(idx));
                if (!reg.all_of<Position>(entity))
                {
                    continue;
                }
                auto &epos = reg.get<Position>(entity);
                const float dx = epos.x - px;
                const float dz = epos.z - pz;
                const float dy = epos.y - py;
                if (dx * dx + dz * dz <= rXzSq && std::abs(dy) <= kViewRadiusY)
                {
                    current.insert(idx);
                }
            }

            // ── 3. 增量 diff ──
            auto &prev = prevState[observerIdx];
            for (uint32 idx : current)
            {
                if (!prev.contains(idx))
                {
                    outEnter.emplace_back(observerIdx, idx);
                }
            }
            for (uint32 idx : prev)
            {
                if (!current.contains(idx))
                {
                    outLeave.emplace_back(observerIdx, idx);
                }
            }
            prev = std::move(current);
        }
    }

} // namespace MMO
```

> **注意**：`Grid::Update(idx, oldX, oldZ, newX, newZ)` 需要旧位置——上面传了相同值导致
> 永远不跨格。**修正**：让 Grid 内部缓存实体旧格（`_entityCell` 已存），`Update` 只需
> `(idx, newX, newZ)`，内部查旧格对比。生产版签名应改为 `Update(uint32 idx, float newX, float newZ)`。

```cpp
// Grid.h 修正签名
void Update(uint32 entityIndex, float newX, float newZ);
// Grid.cpp
void Grid::Update(uint32 entityIndex, float newX, float newZ)
{
    auto it = _entityCell.find(entityIndex);
    if (it == _entityCell.end())
    {
        Insert(entityIndex, newX, newZ);
        return;
    }
    const int64 oldKey = it->second;
    const int ncx = static_cast<int>(std::floor(newX / _cellSize));
    const int ncz = static_cast<int>(std::floor(newZ / _cellSize));
    const int64 newKey = CellKey(ncx, ncz);
    if (oldKey == newKey)
    {
        return;
    }
    Remove(entityIndex);
    Insert(entityIndex, newX, newZ);
}
```

---

## 5. `ActiveSet` — 活跃集（休眠优化）

```cpp
// Src/Common/ECS/ActiveSet.h
#pragma once

#include <unordered_set>

#include "Common/Core/Types.h"

namespace MMO::ECS
{

    /**
     * @brief 活跃实体集——AOI 内或正在模拟的实体
     *
     * 用途：
     *   - Movement 只积分活跃实体（排除 DormantTag）
     *   - 复制只打包活跃实体
     *   - 脚本事件只派发活跃实体相关
     *
     * 实现：观察者（玩家）可见并集 + 显式标记的常驻实体。
     */
    class ActiveSet
    {
    public:
        void Add(uint32 index) { _active.insert(index); }
        void Remove(uint32 index) { _active.erase(index); }
        bool Contains(uint32 index) const { return _active.contains(index); }

        /**
         * @brief 由 AOI 事件流更新：enter 加，leave 减（玩家自身恒活跃）
         */
        void ApplyAOIEvents(const std::vector<std::pair<uint32, uint32>> &enters,
                            const std::vector<std::pair<uint32, uint32>> &leaves)
        {
            for (auto &[obs, idx] : enters)
            {
                _active.insert(idx);
            }
            for (auto &[obs, idx] : leaves)
            {
                _active.erase(idx);
            }
        }

        size_t Count() const { return _active.size(); }
        void Clear() { _active.clear(); }

        // 遍历
        auto begin() const { return _active.begin(); }
        auto end() const { return _active.end(); }

    private:
        std::unordered_set<uint32> _active;
    };

} // namespace MMO::ECS
```

---

## 6. 构建脚本变动

- `Src/Common/ECS/xmake.lua`：`add_files("*.cpp")` 已覆盖 `Grid.cpp`——无需改
- `Src/World/xmake.lua`：`add_files("**.cpp")` 已覆盖 `System/SystemAOI.cpp` + `AOI/`——无需改
- **新建目录**：`Src/World/AOI/`（如用类版本）、`Src/World/System/`（函数版）

---

## 7. 验证步骤（本篇验收）

```powershell
# 1. 构建
xmake build WorldServer

# 2. 功能验证（临时测试代码）：
#    - 玩家 A 在 (0,0,0)，怪物 B 在 (10,0,0)，C 在 (200,0,0)
#    - SystemAOI 后：enter = [(A,B)]，C 不在视野
#    - B 移动到 (150,0,0)：leave = [(A,B)]，enter 无新
#    - 再次 Update（B 静止）：无事件（增量正确）
```

**验收标准**：
- [ ] 构建零错误
- [ ] 增量正确：静止玩家无重复事件
- [ ] 跨格移动正确触发 enter/leave
- [ ] Y 轴过滤正确（|dy| > 15 不出现在视野）
- [ ] `Grid` 去重正确（格子边界实体不重复枚举）

---

## 8. 踩坑预警

1. **`Grid::Update` 签名**：必须用"Grid 缓存旧格"版（`Update(idx, newX, newZ)`），
   不要传 oldX/oldZ——调用方拿不到可靠旧值。
2. **候选去重**：`QueryRadius` 覆盖多格，实体可能在多个格子候选里重复——收集后
   `std::find` 去重（O(n²) 在小候选集可接受；大集合用 `unordered_set` 过渡）。
3. **玩家自身**：`current` 里排除 observer 自身（`idx == observerIdx` continue），
   但复制系统要给玩家自己发 spawn（自己可见自己）——**复制阶段单独处理**（ECS_05）。
4. **Y 轴**：格子是 2D（XZ），Y 过滤在精确阶段做——格子边界实体 Y 不同但同格候选
   会被 Y 过滤掉，正确。
5. **事件消费**：`outEnter/outLeave` 每帧被复制系统 + 脚本事件系统消费后清空——
   不要跨帧累积（SystemAOI 内已 clear）。
