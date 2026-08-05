# 阶段调度器与 dt 管线（ECS_03）

> 契约见 `ECS_00` §4.2/§4.4。本篇给出**可照抄实现**：
> `LogicThread` 固定步长改造 → `StageScheduler` → `World::Tick` → `SystemMovement` 重写 → `WorldServer` 接线。
> 交付后：移动积分服务器权威、每 tick 真跑。

---

## 1. 交付清单

| # | 文件 | 动作 |
|---|---|---|
| 1 | `Src/World/LogicThread.h` | 改（`TickCallback` → `void(float)`；加 accumulator 常量） |
| 2 | `Src/World/LogicThread.cpp` | 改（Phase 4 固定步长 accumulator） |
| 3 | `Src/World/StageScheduler.h` | 新建（EStage/SystemFn/StageScheduler） |
| 4 | `Src/World/StageScheduler.cpp` | 新建 |
| 5 | `Src/World/World.h` | 新建（World 聚合：场景 + 调度器 + tick） |
| 6 | `Src/World/World.cpp` | 新建 |
| 7 | `Src/World/System/SystemMovement.h/.cpp` | 新建（活跃集积分） |
| 8 | `Src/World/WorldServer.h/.cpp` | 改（`OnTick(float)` + `World` 成员） |
| 9 | `Src/World/SceneManager.h/.cpp` | 重建（多场景管理，供 World 使用） |

---

## 2. `LogicThread` 固定步长改造

### 2.1 `LogicThread.h`

```cpp
// 修改 TickCallback 签名
using TickCallback = std::function<void(float dtSeconds)>;

// 在常量区追加（保留 kTickInterval/kMaxElapsed 原值）
/** @brief 固定模拟步长（秒）——20ms = 0.02f，模拟 dt 唯一真相源 */
static constexpr float  kFixedDeltaTime  = 0.02f;
/** @brief 单帧追帧上限——防死亡螺旋 */
static constexpr uint32 kMaxCatchUpSteps = 3;

// 私有成员追加
float _accumulator = 0.0f;
```

> **注意**：`Types.h` 里 `kTickInterval = 50ms`（20 ticks/s）与 `kFixedDeltaTime = 0.02f`（50Hz）
> **冲突**。处理：`LogicThread` 内部睡眠节奏维持 20ms（50Hz），`kFixedDeltaTime = 0.02f` 就是
> 20ms——**两者一致**。`Types.h` 的 `kTickInterval = 50ms` 是旧常量（20 ticks/s = 50ms），
> 实际 LogicThread 用的是自己的 `kTickInterval = 20ms`。为避免混乱，本篇把 `Types.h` 的
> `kTickInterval` 从 50ms 改为 20ms（见 §6 构建变动）。

### 2.2 `LogicThread.cpp` Phase 4 替换

**BEFORE（原 Phase 4）**：

```cpp
            // ── Phase 4: 游戏逻辑（传剩余预算给业务层） ──
            auto now     = std::chrono::steady_clock::now();
            auto elapsed = now - lastTime;
            if (elapsed > kMaxElapsed)
            {
                elapsed = kMaxElapsed;
            }
            lastTime = now;

            auto usedInTick = std::chrono::steady_clock::now() - tickStart;
            auto budget     = kTickInterval - usedInTick;
            if (budget < std::chrono::milliseconds(5))
            {
                budget = std::chrono::milliseconds(5);
            }

            onTick(std::chrono::duration_cast<std::chrono::milliseconds>(budget));
```

**AFTER（替换为固定步长）**：

```cpp
            // ── Phase 4: 固定步长模拟（accumulator 驱动）──
            // 真实耗时累积进 _accumulator，按 kFixedDeltaTime 整步拆分驱动 onTick。
            // dt 恒为 kFixedDeltaTime——积分/复制拿到稳定步长，与实际帧抖动解耦。
            auto now         = std::chrono::steady_clock::now();
            auto realElapsed = now - lastTime;
            lastTime         = now;
            if (realElapsed > kMaxElapsed)
            {
                realElapsed = kMaxElapsed; // 卡顿保护：最多补 50ms
            }

            _accumulator += std::chrono::duration<float>(realElapsed).count();

            uint32 steps = 0;
            while (_accumulator >= kFixedDeltaTime && steps < kMaxCatchUpSteps)
            {
                onTick(kFixedDeltaTime);
                _accumulator -= kFixedDeltaTime;
                ++steps;
            }
            if (_accumulator >= kFixedDeltaTime)
            {
                // 追帧上限达成仍欠账——丢弃时间（防死亡螺旋）
                _accumulator = 0.0f;
                Log::Warn("LogicThread: catch-up limit hit, dropping accumulated time");
            }
```

---

## 3. `StageScheduler` — 8 阶段调度

```cpp
// Src/World/StageScheduler.h
#pragma once

#include <array>
#include <functional>
#include <string_view>

#include "Common/Core/Types.h"

namespace MMO
{

    /** @brief 8 阶段枚举（顺序固定，见 ECS_00 §4.4） */
    enum class EStage : uint8
    {
        PreUpdate     = 0,
        ScriptLogic   = 1,
        Movement      = 2,
        SpatialIndex  = 3,
        AOI           = 4,
        EventDispatch = 5,
        PostUpdate    = 6,
        Replicate     = 7,
        Count         = 8,
    };

    inline constexpr std::string_view kStageName(EStage s)
    {
        switch (s)
        {
            case EStage::PreUpdate:     return "PreUpdate";
            case EStage::ScriptLogic:   return "ScriptLogic";
            case EStage::Movement:      return "Movement";
            case EStage::SpatialIndex:  return "SpatialIndex";
            case EStage::AOI:           return "AOI";
            case EStage::EventDispatch: return "EventDispatch";
            case EStage::PostUpdate:    return "PostUpdate";
            case EStage::Replicate:     return "Replicate";
            default:                    return "?";
        }
    }

    /** @brief 阶段回调签名（无场景参数——场景由调用方捕获） */
    using SystemFn = std::function<void(float dt)>;

    /**
     * @brief 8 阶段调度器——按固定顺序运行每个阶段注册的全部系统
     *
     * 阶段内按注册顺序执行；同一阶段系统间不保证隔离（可读写同一场景）。
     * 场景间并行由外部 World 层驱动（每场景一线程各自调用 RunStage）。
     */
    class StageScheduler
    {
    public:
        /**
         * @brief 注册系统到阶段
         * @param stage  阶段
         * @param name   系统名（诊断用）
         * @param fn     回调
         */
        void Register(EStage stage, std::string_view name, SystemFn fn);

        /**
         * @brief 运行单个阶段
         * @param stage 阶段
         * @param dt    固定步长
         */
        void RunStage(EStage stage, float dt);

        /** @brief 运行全部 8 阶段（按顺序） */
        void RunAll(float dt);

        /** @brief 阶段系统数（诊断） */
        size_t Count(EStage stage) const;

    private:
        struct Entry
        {
            std::string name;
            SystemFn    fn;
        };

        std::array<std::vector<Entry>, static_cast<size_t>(EStage::Count)> _stages;
    };

} // namespace MMO
```

```cpp
// Src/World/StageScheduler.cpp
#include "World/StageScheduler.h"

#include "Common/Log/Log.h"

namespace MMO
{

    void StageScheduler::Register(EStage stage, std::string_view name, SystemFn fn)
    {
        const auto idx = static_cast<size_t>(stage);
        if (idx >= _stages.size() || !fn)
        {
            Log::Error("StageScheduler: invalid register stage={} name={}", idx, name);
            return;
        }
        _stages[idx].push_back({std::string(name), std::move(fn)});
    }

    void StageScheduler::RunStage(EStage stage, float dt)
    {
        const auto idx = static_cast<size_t>(stage);
        if (idx >= _stages.size())
        {
            return;
        }
        for (auto &entry : _stages[idx])
        {
            entry.fn(dt);
        }
    }

    void StageScheduler::RunAll(float dt)
    {
        for (size_t i = 0; i < _stages.size(); ++i)
        {
            RunStage(static_cast<EStage>(i), dt);
        }
    }

    size_t StageScheduler::Count(EStage stage) const
    {
        return _stages[static_cast<size_t>(stage)].size();
    }

} // namespace MMO
```

---

## 4. `World` — 场景 + 调度器聚合

```cpp
// Src/World/World.h
#pragma once

#include <memory>
#include <vector>

#include "Common/ECS/Scene.h"
#include "World/StageScheduler.h"

namespace MMO
{

    /**
     * @brief World——单个模拟域（一个场景）
     *
     * 铁律 3：每场景 1 World = 1 Scene(registry+身份) + 1 调度器。
     * 多场景 = 多 World 实例，各跑各的模拟线程（外部驱动）。
     */
    class World
    {
    public:
        explicit World(uint16 sceneId);

        World(const World &)            = delete;
        World &operator=(const World &) = delete;

        /**
         * @brief 初始化：创建场景 + 注册全部系统
         */
        void Init();

        /**
         * @brief 单帧 tick：按顺序跑 8 阶段
         * @param dt 固定步长（kFixedDeltaTime）
         */
        void Tick(float dt);

        ECS::Scene &Scene() { return *_scene; }
        const ECS::Scene &Scene() const { return *_scene; }
        StageScheduler &Scheduler() { return _scheduler; }
        uint16 SceneId() const { return _sceneId; }

    private:
        uint16         _sceneId;
        std::unique_ptr<ECS::Scene> _scene;
        StageScheduler _scheduler;
    };

} // namespace MMO
```

```cpp
// Src/World/World.cpp
#include "World/World.h"

#include "World/System/SystemMovement.h"

namespace MMO
{

    World::World(uint16 sceneId) : _sceneId(sceneId), _scene(std::make_unique<ECS::Scene>(sceneId))
    {
    }

    void World::Init()
    {
        // 注册系统（按阶段）
        // Movement 阶段：移动积分
        _scheduler.Register(EStage::Movement, "SystemMovement", [this](float dt) {
            SystemMovement(_scene->Registry(), dt);
        });

        // 其余阶段（SpatialIndex/AOI/EventDispatch/Replicate）在 ECS_04/05/06 接入
        Log::Info("World: scene {} initialized", _sceneId);
    }

    void World::Tick(float dt)
    {
        _scheduler.RunAll(dt);
    }

} // namespace MMO
```

---

## 5. `SystemMovement` 重写（活跃集积分）

```cpp
// Src/World/System/SystemMovement.h
#pragma once

#include <entt/entt.hpp>

namespace MMO
{

    /**
     * @brief 移动积分：Position += Velocity * dt
     *
     * 排除 DeadTag/DormantTag（休眠实体不积分）。
     * 写 Position 后标记脏（复制系统消费）。
     *
     * @param reg EnTT registry
     * @param dt  固定步长
     */
    void SystemMovement(entt::registry &reg, float dt);

} // namespace MMO
```

```cpp
// Src/World/System/SystemMovement.cpp
#include "World/System/SystemMovement.h"

#include "Common/ECS/DirtyIndex.h"
#include "Common/Log/Log.h"
#include "World/Component/Position.h"
#include "World/Component/Tags.h"
#include "World/Component/Velocity.h"

namespace MMO
{

    void SystemMovement(entt::registry &reg, float dt)
    {
        auto view = reg.view<Position, Velocity>(entt::exclude<DeadTag, DormantTag>);
        for (auto [e, pos, vel] : view.each())
        {
            pos.x += vel.vx * dt;
            pos.y += vel.vy * dt;
            pos.z += vel.vz * dt;
        }
    }

} // namespace MMO
```

> **注意**：此处 `SystemMovement` 直接操作 `entt::registry`，**不**标记脏（Movement 每帧全量
> 积分，Position 复制走"每玩家 AOI 内的活跃实体全量打包"，不依赖 dirty——见 ECS_05 决策）。

---

## 6. 构建脚本变动

### 6.1 `Src/Common/Core/Types.h`

```cpp
// 统一模拟节奏：kTickInterval 50ms → 20ms（50Hz）
inline constexpr auto kTickInterval = std::chrono::milliseconds(20);
```

### 6.2 `Src/World/xmake.lua`

```lua
-- add_files("**.cpp") 自动覆盖新增 StageScheduler.cpp / World.cpp / System/*.cpp——无需改
-- 但确保 System/ 目录存在（新建 SystemMovement）
```

### 6.3 `Config/world.toml`（场景参数扩展）

```toml
[world]
id = 1
max_players = 10000
persistent_scenes = ["1"]
```

> 场景参数（视野半径/格子大小）由 `SceneManager` 用默认值（`SceneConfig`），
> 后续按需从 toml 读取。MVP 顺序不改 toml 格式。

---

## 7. `WorldServer` 接线

```cpp
// WorldServer.h
#include "World/World.h"

// 私有成员追加：
std::vector<std::unique_ptr<World>> _worlds; // 每场景一个 World

// OnTick 签名修改：
void OnTick(float dtSeconds);
```

```cpp
// WorldServer.cpp

// Init 中（_sceneMgr 加载场景后）：
void WorldServer::InitWorlds()
{
    for (uint32 sceneId : _config.world.persistentScenes)
    {
        auto world = std::make_unique<World>(static_cast<uint16>(sceneId));
        world->Init();
        _worlds.push_back(std::move(world));
    }
    Log::Info("WorldServer: {} worlds initialized", _worlds.size());
}

// OnTick（固定步长回调）：
void WorldServer::OnTick(float dt)
{
    // 1. 消息/控制（既有逻辑）
    ProcessUnroutedMessages();
    ProcessControlMessages();

    // 2. 每个 World tick（单场景单线程；多场景后续并行）
    for (auto &world : _worlds)
    {
        world->Tick(dt);
    }

    // 3. 过载保护（既有逻辑）
    size_t queueDepth = 0;
    {
        std::shared_lock lock(_sessionsMtx);
        for (auto &[sid, ws] : _sessions)
        {
            queueDepth += ws.inbox.SizeApprox();
        }
    }
    UpdateLoadLevel(_sessions.size(), queueDepth);
}
```

> **注意**：`WorldServer::Init` 里 `_sceneMgr` 的 `LoadPersistentScenes` 在 ECS_01 时被移除，
> 本篇由 `InitWorlds()` 替代。`SceneManager` 不再直接持有 `Scene`（改由 `World` 持有），
> 或保留 `SceneManager` 但内部换 `World`——**推荐直接删 `SceneManager`，WorldServer 持有
> `_worlds`**（更清晰，一个 World 就是一个场景）。

---

## 8. 验证步骤（本篇验收）

```powershell
# 1. 构建
xmake build WorldServer

# 2. 启动
#    预期日志：
#    - "World: scene 1 initialized"
#    - "WorldServer: 1 worlds initialized"
#    - 无 "catch-up limit hit" 持续刷屏（正常抖动偶尔一次可接受）

# 3. 功能验证（临时测试代码）：
#    - emplace Position(0,0,0) + Velocity(10,0,0)
#    - 连续 Tick 10 次 → Position.x == 10 * 0.02 * 10 = 2.0
```

**验收标准**：
- [ ] 构建零错误
- [ ] `OnTick(float)` 收到恒等于 `kFixedDeltaTime` 的 dt
- [ ] Movement 积分正确（Position = Velocity * dt * steps）
- [ ] 卡顿追帧上限生效（不掉帧拖时间）
- [ ] `Types.h` kTickInterval=20ms 后，LogicThread 睡眠节奏正常

---

## 9. 踩坑预警

1. **`OnTick` 签名变化**：`std::chrono::milliseconds` → `float`，所有调用点（`LogicThread` lambda）
   要同步。lambda 里 `[this](float dt) { OnTick(dt); }`。
2. **accumulator 精度**：`std::chrono::duration<float>` 累积浮点误差可忽略（每帧 <1μs）。
   追帧上限命中时**丢弃**而非欠账——否则卡顿后一直追帧（死亡螺旋）。
3. **`entt::exclude<DeadTag, DormantTag>`**：Exclude 语法 EnTT 3.16 是 `entt::exclude<T>` 传入 view
   构造——不是 `view<T>(entt::exclude<...>)` 就是 `reg.view<A, B>(entt::exclude<C>)`，确认项目
   已用此语法（旧 System.cpp 即如此），保持一致。
4. **World 生命周期**：`_worlds` 在 `WorldServer::Init` 创建，`Stop` 前销毁（或随成员析构）。
   多场景并行（每场景一线程）在后续版本加——**本篇先串行**（WorldServer 主循环单线程 tick 所有 world）。
