# 阶段调度器与 dt 管线（ECS_03_StageScheduler）

> 本篇是 ECS 系列第 3 篇（P1「tick 接线」）。目标只有一个：**让 `ECS_00 §4` 定义的 8 阶段管线真正跑起来**。
> 当前整条模拟管线是死代码——`WorldServer::OnTick`（`WorldServer.cpp:238`）只做消息/控制/负载三件事，
> `SystemMovement`/`RunCPPSystems`/`SystemReplicate` 全部零调用点（源码 grep 确认）。
> 本篇给出：① `LogicThread` 的 dt 交付改造（固定步长 accumulator）；② `SceneManager` 缺失的场景遍历 API；
> ③ `StageScheduler` + `World::Tick(dt)` 骨架与 `WorldServer::OnTick` 接线；④ `SystemMovement` 重写为活跃集积分。
> 交付后：移动积分服务器权威、每 tick 真跑。契约（uint64、命名、8 阶段顺序、5 个已定决策）一律以 `ECS_00` 为准，本篇不重述。
>
> 前置依赖：P0（`ECS_01` 身份系统）已落地；组件 `DormantTag`/`Velocity` 由 `ECS_02` 提供。
> 空间索引/AOI（阶段 4/5）见 `ECS_04`；复制（阶段 8）见 `ECS_05`；脚本 `[game_system]`/`[game_event]`（阶段 2/6 内容）见 `ECS_07`。

---

## 0. 本篇改动总览

| # | 文件 | 改动 | 性质 |
|---|---|---|---|
| 1 | `Src/World/LogicThread.h` | `TickCallback` 改 `void(float)`；加 `kFixedDeltaTime`/`kMaxCatchUpSteps`；加 `_accumulator` 成员 | 改 |
| 2 | `Src/World/LogicThread.cpp` | `RunLoop` Phase 4 改固定步长 accumulator（追帧上限 3） | 改 |
| 3 | `Src/World/SceneManager.h` | 新增 `ForEachScene(fn)` 模板遍历 API | 加 |
| 4 | `Src/World/StageScheduler.h`（新建） | `EStage` / `SystemFn` / `StageScheduler` / `World` | 新建 |
| 5 | `Src/World/StageScheduler.cpp`（新建） | `StageScheduler::Register/RunStage`、`World::Init/RegisterSystem/Tick` | 新建 |
| 6 | `Src/World/WorldServer.h` | `OnTick(std::chrono::milliseconds)` → `OnTick(float)`；加 `World _world` 成员 | 改 |
| 7 | `Src/World/WorldServer.cpp` | `Init` 接线 `World`；`OnTick` 调 `_world.Tick(dt)`；lambda 改 float | 改 |
| 8 | `Src/World/System/System.cpp` | `SystemMovement` 重写为活跃集积分（`exclude<DeadTag, DormantTag>`） | 改 |

> 新建的两个文件位于 `Src/World/`，随 `WorldServer` target 纳入编译（构建细节见 `ECS_08`）。

---

## 1. dt 管线：LogicThread 固定步长改造

### 1.1 为什么必须改

已核实事实（见提取报告 `tick-session`）：`RunLoop` 计算了 `elapsed = now - lastTime` 并 clamp 到 `kMaxElapsed`，
但**算完即丢**（只留 `lastTime = now`），传给 `onTick` 的是「剩余预算 budget（5~20ms）」而非 dt。
所以业务层拿不到帧间隔，移动积分无从谈起。`ECS_00 §6.2` 已拍板：改 `TickCallback` 为 `void(float dtSeconds)`，
`RunLoop` 用**固定步长 accumulator** 驱动，追帧上限 3 步防死亡螺旋；`_currentMsgLimit` 过载反馈**保留但与 dt 解耦**
（它管 `ProcessMessages` 入口，不管模拟步长）。

### 1.2 LogicThread.h：签名 + 常量 + 成员

`TickCallback` typedef（`LogicThread.h:33`）改动：

```cpp
        /** @brief 游戏逻辑回调，参数为固定模拟步长 dtSeconds（恒等于 kFixedDeltaTime） */
        using TickCallback = std::function<void(float dtSeconds)>;
```

在既有常量块（`LogicThread.h:97-99`）追加两个模拟常量，并保留原有 tick 常量不变（`ECS_00 §6.1`：tick 频率维持 20ms/50Hz）：

```cpp
        static constexpr size_t kMaxMessagesPerTick = 1000;
        static constexpr auto   kTickInterval       = std::chrono::milliseconds(20);
        static constexpr auto   kMaxElapsed         = std::chrono::milliseconds(50);

        /** @brief 固定模拟步长（秒）——20ms = 0.02f，模拟 dt 唯一真相源（ECS_00 §6.1） */
        static constexpr float  kFixedDeltaTime     = 0.02f;

        /** @brief 单帧追帧上限——防止卡顿后 while 循环失控形成死亡螺旋（ECS_00 §6.2） */
        static constexpr uint32 kMaxCatchUpSteps    = 3;
```

在私有成员区（紧邻 `_currentMsgLimit`）加一个累加器成员：

```cpp
        // 动态入口门控——根据 Tick 负载自动调整每 Tick 处理的消息数
        uint32 _currentMsgLimit = kMaxMessagesPerTick;

        // 固定步长模拟累加器（秒）：跨帧累积真实耗时，按 kFixedDeltaTime 拆成整步驱动 onTick
        float _accumulator = 0.0f;
```

### 1.3 RunLoop 的精确 before/after

改动只集中在 **Phase 4**（原 `LogicThread.cpp:87-104`），其余 Phase 0/1/2/3/6/7 与 sleep 逻辑逐字不动。

**BEFORE（`LogicThread.cpp:87-104` 逐字）**：

```cpp
            // ── Phase 4: 游戏逻辑（传剩余预算给业务层） ──
            auto now     = std::chrono::steady_clock::now();
            auto elapsed = now - lastTime;
            if (elapsed > kMaxElapsed)
            {
                elapsed = kMaxElapsed;
            }
            lastTime = now;

            // 计算剩余预算：离 20ms 还有多少时间，给业务层自己决定做多少事
            auto usedInTick = std::chrono::steady_clock::now() - tickStart;
            auto budget     = kTickInterval - usedInTick;
            if (budget < std::chrono::milliseconds(5))
            {
                budget = std::chrono::milliseconds(5); // 最少给 5ms
            }

            onTick(std::chrono::duration_cast<std::chrono::milliseconds>(budget));
```

**AFTER（替换上述整段）**：

```cpp
            // ── Phase 4: 固定步长模拟（accumulator 驱动，防死亡螺旋）──
            // 真实耗时累积进 _accumulator，按 kFixedDeltaTime 整步拆分驱动 onTick，
            // dt 恒为 kFixedDeltaTime——积分/复制拿到的永远是稳定步长，与实际帧抖动解耦
            auto now         = std::chrono::steady_clock::now();
            auto realElapsed = now - lastTime;
            lastTime         = now;
            if (realElapsed > kMaxElapsed)
            {
                // 卡顿保护：一次最多补 50ms，超出的时间直接丢弃，不追补丢失帧
                realElapsed = kMaxElapsed;
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
                // 追帧上限用尽仍有积压 → 丢弃并告警，防止 accumulator 越滚越大
                Log::Warn("LogicThread: sim overloaded, dropping {}ms accumulated",
                          static_cast<int32>(_accumulator * 1000.0f));
                _accumulator = 0.0f;
            }
```

**要点**：

- `onTick(kFixedDeltaTime)` 每帧被调用 **0~3 次**：正常帧恰好 1 次（20ms≈1 步）；追帧最多 3 步；极端卡顿丢弃余额。
- 原 `budget` 局部与它对 `onTick` 的传参一并删除——业务层不再需要「剩余预算」。
- `_currentMsgLimit` 的过载反馈（Phase 0 读、Phase 7 调整）**原样保留**：它基于整帧 `tickCost` 墙钟时间，
  与 dt/步长完全无关（兑现 `ECS_00 §6.2` 的「与 dt 解耦」）。Phase 7 那段 `if (tickCost > kTickInterval * 0.8)`
  逐字不动。
- `kTickInterval`/`kMaxElapsed` 保持不变，sleep-to-next-tick 逻辑（`LogicThread.cpp:129-141`）逐字不动。

---

## 2. SceneManager：新增场景遍历 API

`StageScheduler` 要对**每个**场景跑阶段，但 `SceneManager` 当前只有 `GetScene`/`GetDefaultScene`/`Count`——
**没有任何遍历入口**（`_scenes` 私有，`GetDefaultScene` 只 `return _scenes.begin()->second.get()`，源码确认）。
补一个 header-only 模板遍历器，零 cpp 改动、零虚调用开销：

在 `SceneManager` 的 public 区（`Count()` 之后）新增：

```cpp
        /**
         * @brief 遍历所有场景（StageScheduler 逐场景跑阶段用）
         * @tparam Fn  可调用对象，签名 void(ECS::Scene &)
         * @param  fn  对每个场景调用一次
         *
         * @note 仅 LogicThread 单线程调用；遍历期间不得增删场景（无迭代器失效风险）。
         */
        template <typename Fn>
        void ForEachScene(Fn &&fn)
        {
            for (auto &kv : _scenes)
            {
                fn(*kv.second);
            }
        }
```

> 命名遵循 `ECS_00 §3.2`：`sceneID` 若需暴露一律大写 ID；此处只遍历 value，故不引入未用绑定。

---

## 3. StageScheduler + World::Tick

### 3.1 阶段枚举与系统函数指针

8 阶段顺序写死（`ECS_00 §4` 契约，不可增删不可乱序），阶段内 system 列表可注册。
新建 `Src/World/StageScheduler.h`：

```cpp
/**
 * @file StageScheduler.h
 * @brief 8 阶段模拟管线调度器 + World::Tick 入口
 *
 * 阶段顺序写死（ECS_00 §4）：PreUpdate → ScriptLogic → Movement → SpatialIndex
 *   → AOI → EventDispatch → PostUpdate ──屏障── Replicate。
 * 阶段 1~7 在 LogicThread 串行执行（写组件无锁）；阶段 8 复制在屏障后只读并行（见 ECS_05）。
 */
#pragma once

#include <array>
#include <vector>

#include "Common/Core/Types.h"

namespace MMO
{
    class SceneManager;
} // namespace MMO

namespace MMO::ECS
{
    class Scene;
} // namespace MMO::ECS

namespace MMO
{

    /**
     * @brief 模拟管线阶段（ECS_00 §4，顺序即枚举值，不可改）
     */
    enum class EStage : uint8
    {
        PRE_UPDATE     = 0, // 消息 handler 产生的指令
        SCRIPT_LOGIC   = 1, // daslang [game_system]（脚本驱动，非 C++ SystemFn）
        MOVEMENT       = 2, // C++ 移动积分
        SPATIAL_INDEX  = 3, // C++ 格子索引更新（ECS_04）
        AOI            = 4, // C++ 增量 AOI（ECS_04）
        EVENT_DISPATCH = 5, // 游戏事件出队 → daslang [game_event]（ECS_07）
        POST_UPDATE    = 6, // dirty 收集、死亡清理、休眠判定
        REPLICATE      = 7, // [并行] 每玩家复制打包（ECS_05）
    };

    /** @brief 阶段总数 */
    inline constexpr uint32 kStageCount = 8;

    /**
     * @brief C++ 逐场景系统函数指针
     * @param scene  目标场景
     * @param dt     固定步长（恒等于 LogicThread::kFixedDeltaTime）
     *
     * @note 仅用于纯 C++ 逐场景阶段（Movement/SpatialIndex/PostUpdate 等）。
     *       ScriptLogic/EventDispatch/Replicate 因签名不同（脚本 / 跨 session）由 World 特化调用，不走此列表。
     */
    using SystemFn = void (*)(ECS::Scene &scene, float dt);
```

### 3.2 StageScheduler 类

紧接上文，同一头文件：

```cpp
    /**
     * @brief 阶段调度器：每阶段持有一组 C++ SystemFn，按注册顺序执行
     */
    class StageScheduler
    {
    public:
        /**
         * @brief 向某阶段注册一个 C++ 系统
         * @param stage  目标阶段
         * @param fn     系统函数指针（非空）
         */
        void Register(EStage stage, SystemFn fn);

        /**
         * @brief 对单个场景执行某阶段的全部已注册系统
         * @param stage  阶段
         * @param scene  场景
         * @param dt     固定步长
         */
        void RunStage(EStage stage, ECS::Scene &scene, float dt);

    private:
        std::array<std::vector<SystemFn>, kStageCount> _stages;
    };
```

### 3.3 World 类

`World` 是模拟世界的调度门面：持有 `StageScheduler`，引用 `SceneManager`，对外只暴露 `Tick(dt)`。
`WorldServer` 持有一个 `World` 成员并在 `OnTick` 里调它。仍在同一头文件：

```cpp
    /**
     * @brief 模拟世界调度门面——8 阶段管线的唯一驱动入口
     *
     * 由 WorldServer 持有；WorldServer::OnTick 每步调用一次 Tick(dt)。
     * 阶段 1~7 逐场景串行；阶段 8 复制由 WorldServer 在屏障后特化调用（ECS_05）。
     */
    class World
    {
    public:
        /**
         * @brief 绑定场景管理器
         * @param sceneMgr  WorldServer::_sceneMgr（生命周期长于 World）
         */
        void Init(SceneManager *sceneMgr);

        /**
         * @brief 注册一个 C++ 逐场景系统到指定阶段
         */
        void RegisterSystem(EStage stage, SystemFn fn);

        /**
         * @brief 驱动一步固定步长模拟（阶段 1~7）
         * @param dt  固定步长（LogicThread::kFixedDeltaTime）
         */
        void Tick(float dt);

        /**
         * @brief 标记脚本层已就绪（provider 已构造 + Initialize 成功）
         *
         * P1 不接脚本，`_scriptReady` 恒为 false，`Tick` 跳过 `DasLangEngine::Tick`。
         * P4（ECS_06 接好 WorldDasModule provider）后由 WorldServer::InitScriptEngine 置 true。
         */
        void SetScriptReady(bool ready)
        {
            _scriptReady = ready;
        }

    private:
        SceneManager  *_sceneMgr = nullptr;
        StageScheduler _scheduler;
        bool           _scriptReady = false;
    };

} // namespace MMO
```

新建 `Src/World/StageScheduler.cpp`：

```cpp
/**
 * @file StageScheduler.cpp
 * @brief StageScheduler / World 实现
 */

#include "World/StageScheduler.h"

#include "Common/Core/MassiveAssert.h"
#include "Common/ECS/Scene.h"
#include "Common/Log/Log.h"
#include "ScriptEngine/DasEngine.h"
#include "World/SceneManager.h"

namespace MMO
{

    void StageScheduler::Register(EStage stage, SystemFn fn)
    {
        MASSIVE_ASSERT(nullptr != fn, "StageScheduler::Register: null SystemFn");
        _stages[static_cast<size_t>(stage)].push_back(fn);
    }

    void StageScheduler::RunStage(EStage stage, ECS::Scene &scene, float dt)
    {
        for (SystemFn fn : _stages[static_cast<size_t>(stage)])
        {
            fn(scene, dt);
        }
    }

    void World::Init(SceneManager *sceneMgr)
    {
        MASSIVE_ASSERT(nullptr != sceneMgr, "World::Init: null SceneManager");
        _sceneMgr = sceneMgr;
    }

    void World::RegisterSystem(EStage stage, SystemFn fn)
    {
        _scheduler.Register(stage, fn);
    }

    void World::Tick(float dt)
    {
        MASSIVE_ASSERT(nullptr != _sceneMgr, "World::Tick: not initialized");

        // ── 阶段 2 ScriptLogic（含热重载安全点，见 §3.5）──
        // DasLangEngine::Tick 内部先 PollReload（tick 边界换 Context 安全），再 eval 脚本 Update 驱动 [game_system]。
        // 全局调用一次（脚本 Context 单例，跨场景共享），置于所有 C++ 写阶段之前。
        //
        // ⚠ P1 不在此调用 DasLangEngine::Tick —— 见 §3.5「P1 不接脚本 tick」。
        //   脚本层 provider 未接线前调用会崩溃；ScriptLogic 阶段在 P4（ECS_06 接 provider 后）点亮。
        if (_scriptReady)
        {
            DasLangEngine::GetIns().Tick(dt);
        }

        // ── 阶段 1、3~7：逐场景串行 C++ 系统（LogicThread 单线程，写组件无锁）──
        _sceneMgr->ForEachScene([this, dt](ECS::Scene &scene) {
            _scheduler.RunStage(EStage::PRE_UPDATE, scene, dt);     // 1（P4 接指令队列，ECS_07）
            _scheduler.RunStage(EStage::MOVEMENT, scene, dt);       // 3（本篇接 SystemMovement）
            _scheduler.RunStage(EStage::SPATIAL_INDEX, scene, dt);  // 4（P2 接 Grid，ECS_04）
            _scheduler.RunStage(EStage::AOI, scene, dt);            // 5（P2 接增量 AOI，ECS_04）
            _scheduler.RunStage(EStage::EVENT_DISPATCH, scene, dt); // 6（P4 接事件队列，ECS_07）
            _scheduler.RunStage(EStage::POST_UPDATE, scene, dt);    // 7（P2+ 接休眠/清理）
        });

        // ── 屏障：阶段 1~7 结束，组件进入只读快照窗口 ──
        // 阶段 8 Replicate（[并行]、跨 session、需 _sessions/_aoiStates）由 WorldServer 在屏障后特化调用。
        // P1 阶段复制尚未接线（SystemReplicate 与 SendRawToClient 空 stub 均待 P3，见 ECS_05）。
    }

} // namespace MMO
```

> 阶段 4/5/6/7 的 system 列表在 P1 为空——`RunStage` 遍历空 `vector` 即空操作，**可编译可运行**，
> 待 `ECS_04`/`ECS_07` 注册对应系统后自动生效。这不是占位代码：调度框架本身在 P1 完整且正确，
> 只是尚无系统注册进这些阶段（明确的分期，非「TODO」）。

### 3.4 WorldServer 接线

**`WorldServer.h`**：`OnTick` 签名改 float，新增 `World` 成员。

```cpp
        // ── LogicThread 回调 ──
        void OnTick(float dt);                                       // 原：std::chrono::milliseconds elapsed
```

在 `SceneManager _sceneMgr;` 成员之后新增：

```cpp
        // ── 场景 ──
        SceneManager _sceneMgr;

        // ── 模拟世界调度（8 阶段管线）──
        World _world;
```

并加头文件包含（项目头组）：

```cpp
#include "World/StageScheduler.h"
```

**`WorldServer.cpp`**：`Init` 中在 `LoadPersistentScenes` 成功后、`_logicThread.Start` 之前接线 `World`：

```cpp
        // 加载常驻场景
        if (!_sceneMgr.LoadPersistentScenes(cfg.world.persistentScenes))
        {
            Log::Error("WorldServer: no scenes loaded");
            return false;
        }

        // 绑定模拟世界调度器并注册 C++ 系统（阶段 3 Movement）
        _world.Init(&_sceneMgr);
        _world.RegisterSystem(EStage::MOVEMENT, &SystemMovement);
```

`_logicThread.Start` 的 tick lambda（`WorldServer.cpp:91-93`）改为显式 float：

```cpp
        _logicThread.Start(
            &_sessions,
            &_sessionsMtx,
            [this](float dt) {
                OnTick(dt);
            },
```

`OnTick` 实现（`WorldServer.cpp:238`）改签名并在末尾驱动模拟：

```cpp
    void WorldServer::OnTick(float dt)
    {
        // 1. 处理未路由的 EnterWorldReq（内部持 unique_lock 写 _sessions）
        ProcessUnroutedMessages();

        // 2. 控制消息（DisconnectNtf / SessionRebindReq）
        ProcessControlMessages();

        // 3. 驱动 8 阶段模拟管线（阶段 1~7；阶段 8 复制待 P3 接线）
        _world.Tick(dt);

        // 4. 过载保护（读遍历 _sessions，与 IO 线程 shared_lock 并发读不冲突）
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

> 消息/控制先于 `_world.Tick`：本 tick 新入世的实体、新到的移动指令在积分前已就绪；
> 过载统计放最后，读的是排干后的真实积压。

### 3.5 DasLangEngine::Tick 的热重载安全点（阶段 2 / 6）

已核实（`DasEngine.cpp:160-188`）：`DasLangEngine::Tick(float dt)` **本身就是 `float` 签名**（无需改动），
且它把三件事**打包**在一次调用里：

```cpp
    void DasLangEngine::Tick(float dt)
    {
        PollReload();                       // ← 热重载：mtime 命中则 Compile+Simulate+DoSwap 换 Context

        if (!_scriptImage.IsValid()) { return; }

        _moduleProvider->OnPrevTick(dt);
        _scriptImage.ctx->restart();
        if (nullptr != _scriptImage.funcUpdate)
        {
            vec4f args[] = {das::cast<float>::from(dt)};
            _scriptImage.ctx->evalWithCatch(_scriptImage.funcUpdate, args); // ← 脚本 Update = 阶段 2 驱动
        }
        // … 4MB 阈值触发 collectHeap
    }
```

`PollReload`（`DasEngine.cpp:296`）在命中时 `DoSwap` → `_scriptImage = std::move(img)` **直接丢弃旧 Context**
（`ECS_00 铁律 1`）。因此换 Context **只能在 tick 边界**（`ECS_00 §4`：阶段 7 后、下一轮阶段 1 前），
绝不能在阶段中途——否则半途的脚本调用会踩到已析构的旧 Context。

**P1 的落位方案（关键：P1 不接脚本 tick）**：`World::Tick` 里对 `DasLangEngine::GetIns().Tick(dt)` 的调用
**受 `_scriptReady` 守卫，P1 恒为 false，即 P1 完全不调它**。原因是硬约束、非选择：

> `DasLangEngine::Tick`（`DasEngine.cpp:169`）在 `_scriptImage.IsValid()` 为真后**无 null 保护地**
> 调用 `_moduleProvider->OnPrevTick(dt)`（对比 `Initialize` 在 `:78` 有 `if (nullptr != _moduleProvider)` 守卫）。
> 而真实 `WorldServer::_moduleProvider`（`WorldServer.h:194`，`unique_ptr<IDasLangModuleProvider>`）
> **从未被构造**——grep 全树只在 `WorldServer.cpp:620` 以 `.get()` 读取，从无 `make_unique`/`reset` 赋值，恒为 null。
> 一旦 `main.das` 加载成功（`IsValid()` 为真），第一个 tick 就会 null 解引用 `_moduleProvider` 崩溃
> （`BuildModuleGroup` 在 `DasEngine.cpp:382` 也有同样的裸解引用，说明脚本路径当前整体是未接线 WIP）。

因此 P1 的正确做法是**把脚本 tick 排除在 P1 之外**：`World::Tick` 只跑阶段 1、3~7 的 C++ 系统，移动即成为
服务器权威并真正运行。脚本层的接线（构造 `WorldDasModule` provider → `Initialize` → `SetScriptReady(true)`）
在 **P4/`ECS_06`** 完成，届时 `_scriptReady` 置 true，阶段 2 ScriptLogic 才点亮。

热重载边界仍成立：当 P4 打开脚本 tick 后，`DasLangEngine::Tick` 置于 `World::Tick` 首行（所有 C++ 写阶段之前），
上一 tick 已结束、本 tick 尚无组件改动——正是 `PollReload`/`DoSwap` 换 Context 的合法边界。

> **须向编排者上报的偏差**：`ECS_00 §4` 把脚本分成阶段 2（`[game_system]`）与阶段 6（`[game_event]` 事件出队），
> 但真实 `DasLangEngine::Tick` 是 `PollReload + Update` 的单一打包调用，**无法原地拆成阶段 2/6 两个切点**。
> P1 用「Tick 打头」一次性覆盖阶段 2 + 热重载边界，是完整且正确的做法。待 `ECS_07`/P4 引入事件队列与
> `[game_event]` 时，需把 `PollReload`（留在边界）与脚本 `Update`（阶段 2）/事件分发（阶段 6）**拆成独立调用**
> ——那是 `DasEngine` 的后续小改，不影响 P1 交付。

---

## 4. SystemMovement 重写（活跃集积分）

真实实现（`System.cpp:27`，即 `void SystemMovement(ECS::Scene &, float)`）当前只 `exclude<DeadTag>`：

```cpp
    void SystemMovement(ECS::Scene &scene, float dt)
    {
        auto view = scene.Registry().view<Position, Velocity>(entt::exclude<DeadTag>);

        for (auto [e, pos, vel] : view.each())
        {
            pos.x += vel.vx * dt;
            pos.y += vel.vy * dt;
            pos.z += vel.vz * dt;
        }
    }
```

`ECS_00 §6.4` 引入 `DormantTag`（休眠标记），移动积分只应跑**活跃集**——排除死亡与休眠实体。
重写后（`System.cpp`，签名不变）：

```cpp
    void SystemMovement(ECS::Scene &scene, float dt)
    {
        // 活跃集：持有 Position+Velocity、且未死亡（DeadTag）未休眠（DormantTag）的实体才积分。
        // dt 恒为 LogicThread::kFixedDeltaTime（0.02f）——由固定步长调度器传入，此处不硬编码裸数字。
        auto view = scene.Registry().view<Position, Velocity>(entt::exclude<DeadTag, DormantTag>);

        for (auto [e, pos, vel] : view.each())
        {
            pos.x += vel.vx * dt;
            pos.y += vel.vy * dt;
            pos.z += vel.vz * dt;
        }
    }
```

- **EnTT 3.16.0 已核实**：`registry.view<Position, Velocity>(entt::exclude<DeadTag, DormantTag>)` 是合法的
  多组件排除视图；`view.each()` 结构化绑定解出 `(entt::entity, Position&, Velocity&)`——与既有代码同款用法，无 API 杜撰。
- `dt` 参数即 `kFixedDeltaTime`（由 `RunStage → SystemFn` 一路传入），符合 `ECS_00 §6.1`「符号常量表达步长」；
  `SystemMovement` 不 `#include LogicThread.h`（保持 `System` 与 `LogicThread` 无耦合），步长由调用方注入。
- `DormantTag` 由 `ECS_02`（`World/Component/Tags.h`）提供；确保 `System.cpp` 已 `#include "World/Component/Tags.h"`
  （现状已含，`DeadTag` 即来自此）。休眠/唤醒的判定逻辑（谁给实体打 `DormantTag`）在阶段 7 PostUpdate，
  由 `ECS_04` 活跃集章节落地——本篇只保证「已休眠者不参与积分」。

> `RunCPPSystems`（`System.cpp:102`，内部串 `SystemMovement`+`SystemAOI`）在新架构下**不再使用**：
> 阶段调度取代了这个硬编码串联。P1 只把 `SystemMovement` 注册进 `EStage::MOVEMENT`；`SystemAOI` 待 `ECS_04`
> 重写为增量 AOI 后注册进 `EStage::AOI`。`RunCPPSystems` 可保留待删或直接删除（`ECS_08` 清理）。

---

## 5. 迁移步骤清单（可执行）

按序执行，每步都可独立编译；全部完成后移动积分服务器权威、每 tick 真跑。

1. **LogicThread.h**：`TickCallback` 改 `void(float dtSeconds)`；追加 `kFixedDeltaTime=0.02f`、`kMaxCatchUpSteps=3`；
   加成员 `float _accumulator = 0.0f;`（§1.2）。
2. **LogicThread.cpp**：替换 `RunLoop` Phase 4 为固定步长 accumulator（§1.3）；删除 `budget` 传参；
   Phase 0/7 的 `_currentMsgLimit` 反馈与 sleep 逻辑不动。
3. **SceneManager.h**：新增 `ForEachScene(fn)` 模板遍历 API（§2）。
4. **新建 StageScheduler.h / StageScheduler.cpp**：`EStage`/`SystemFn`/`StageScheduler`/`World`（§3.1~3.3）；
   加入 `WorldServer` target 源列表（见 `ECS_08`）。
5. **WorldServer.h**：`OnTick(std::chrono::milliseconds)` → `OnTick(float)`；新增 `World _world;` 成员；
   `#include "World/StageScheduler.h"`（§3.4）。
6. **WorldServer.cpp**：`Init` 中 `_world.Init(&_sceneMgr)` + `_world.RegisterSystem(EStage::MOVEMENT, &SystemMovement)`；
   `Start` 的 tick lambda 改 `[this](float dt)`；`OnTick` 改签名并在消息/控制之后调 `_world.Tick(dt)`（§3.4）。
7. **System.cpp**：`SystemMovement` 改 `entt::exclude<DeadTag, DormantTag>`（§4）；确认 `Tags.h` 含 `DormantTag`（依赖 `ECS_02`）。

**验证方法**：

- **编译**：`xmake build worldserver`（或全量 `xmake`）——签名改动、新文件、EnTT 视图三处若有 API/类型错误即刻暴露。
- **单测**：构造一个 `Scene`，`CreateEntity` 后 `emplace<Position>{0,0,0}` + `emplace<Velocity>{1,0,0}`，
  手动 `World::Tick(0.02f)` 一次，断言 `Position.x ≈ 0.02f`；再给同实体加 `DormantTag`，`Tick` 后断言 `x` 不再变化
  （验证活跃集排除）。追帧：`RunLoop` 中人为注入 `realElapsed = 70ms` 应触发恰好 3 次 `onTick` + 一条 drop 告警。
- **集成跑**：启动 `WorldServer`，客户端进世界发 `MoveReq` 给实体设速度，观察日志 `LogicThread: started`
  且**无** `sim overloaded` 刷屏；确认实体 `Position` 随 tick 线性推进（复制可视化待 P3，此处看服务端日志/调试器即可）。

> 完成 P1 后，管线骨架已通电：阶段 3 Movement 真跑，阶段 2 脚本安全点就位，阶段 4/5/8（AOI/复制）
> 留好注册位待 `ECS_04`/`ECS_05` 接入。下一步进入 P2（`ECS_04`：Grid + 增量 AOI + 活跃集休眠）。
