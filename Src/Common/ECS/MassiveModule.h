/**
 * @file MassiveModule.h
 * @brief DasLang C++ Module — 脚本层桥接入口
 *
 * 暴露 15 个窄接口函数给 DasLang 脚本，分 5 组：
 *   空间查询、属性查询、Tag/State 判断、世界交互、定时器。
 *
 * Phase 1: LogInfo/LogWarn/LogError — 验证编译链路 ✅
 * Phase 2: 完整 15 函数 — EnTT ↔ DasLang 双向操作
 */
#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>

#include <daScript/das_common.h>
#include <daScript/simulate/simulate.h>
#include <daScript/daScriptModule.h>
#include <daScript/ast/ast_interop.h>

#include "Common/Core/Types.h"

// 前向声明——不引入 ECS / World 头文件
namespace MMO
{
    class WorldServer;
    class SceneManager;
    class TimingWheel;
    struct WorldSession;
    struct BattleStats;
} // namespace MMO

namespace MMO
{

    /**
     * @brief DasLang Module "massive" — 脚本与 C++ 世界的唯一通道
     *
     * 持有 4 个上下文指针，桥接函数通过它们访问 EnTT registry / sessions / 定时器。
     */
    class MassiveModule : public das::Module
    {
    public:
        MassiveModule() = default;

        /**
         * @brief 构造并注册全部桥接函数
         * @param worldServer   WorldServer 实例（供 SendRawToClient）
         * @param sceneMgr      场景管理器（供 GetScene / GetDefaultScene）
         * @param timingWheel   LogicThread 独占定时器（供 ScheduleTimer）
         * @param sessions      WorldServer::_sessions 引用（供 FindEntityBySession）
         */
        MassiveModule(WorldServer                               *worldServer,
                      SceneManager                              *sceneMgr,
                      TimingWheel                               *timingWheel,
                      std::unordered_map<uint32, WorldSession>  *sessions);

        ~MassiveModule() override;

        /**
         * @brief 注册所有桥接函数到 DasLang Module
         *
         * 在构造后、addModule 到 ModuleGroup 前调用。
         */
        void BindFunctions();

        /**
         * @brief 获取 DasLang Context（供 ScheduleTimer 回调使用）
         */
        das::Context *GetContext() const
        {
            return _ctx.get();
        }

        // ── 桥接函数访问区（public — .cpp 中 static 桥接函数通过 g_massiveMod 指针访问）──
        // 这些指针由构造函数注入，Init/Script Tick 阶段均为只读，无并发问题。

        WorldServer                              *_worldServer = nullptr;
        SceneManager                             *_sceneMgr    = nullptr;
        TimingWheel                              *_timingWheel = nullptr;
        std::unordered_map<uint32, WorldSession> *_sessions    = nullptr;

        std::shared_ptr<das::Context> _ctx;

        /// CodeReview #4: 每帧由 OnTick 更新，Bridge_GetDeltaTime 读取
        std::atomic<float> _scriptDt{0.02f};

        struct TimerCallback
        {
            das::TBlock<void, uint32_t>     block;
            std::shared_ptr<das::Context>   ctx;
        };
        std::unordered_map<uint32_t, TimerCallback> _timerCallbacks;
        std::atomic<uint32_t>                        _nextTimerID {1};
    };

} // namespace MMO
