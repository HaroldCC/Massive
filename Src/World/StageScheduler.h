#pragma once

#include "COmmon/Core/Types.h"
#include <functional>
#include <array>
#include <string_view>

namespace MMO
{
    // 阶段
    enum class EStage : uint8
    {
        PreUpdate     = 0, // 消息入队处理、EnterWorld、断线清理
        ScriptLogic   = 1, // 低频决策（AI/技能/Buff）→ 事件出队
        Movement      = 2, // 移动积分 Position += Velocity * dt
        SpatialIndex  = 3, // 更新格子索引（增量）
        AOI           = 4, // 增量 AOI，产出入/出事件
        EventDispatch = 5, // 事件队列 → daslang [game_event]
        PostUpdate    = 6, // 死亡清理、DirtyIndex 汇总
        Replicate     = 7, // 只读快照 → 并行打包 → 加密 → Gate
        Count         = 8,
    };

    inline constexpr std::string_view StageName(EStage stage)
    {
        switch (stage)
        {
            case EStage::PreUpdate:
                return "PreUpdate";
            case EStage::ScriptLogic:
                return "ScriptLogic";
            case EStage::Movement:
                return "Movement";
            case EStage::SpatialIndex:
                return "SpatialIndex";
            case EStage::AOI:
                return "AOI";
            case EStage::EventDispatch:
                return "EventDispatch";
            case EStage::PostUpdate:
                return "PostUpdate";
            case EStage::Replicate:
                return "Replicate";
            default:
                return "Unknown";
        }
    }

    // 阶段回调函数
    using SystemFunc = std::function<void(float dtSeconds)>;

    class StageScheduler
    {
    public:
        /**
         * @brief 注册系统到阶段
         * @param stage  阶段
         * @param name   系统名（诊断用）
         * @param func     回调
         */
        void RegisterSystem(EStage stage, std::string_view name, SystemFunc func);

        /**
         * @brief 运行单个阶段
         * @param stage 阶段
         * @param dtSeconds 固定步长
         */
        void RunStage(EStage stage, float dtSeconds);

        /**
         * @brief 运行所有阶段
         * @param dtSeconds 固定步长
         */
        void RunAllStage(float dtSeconds);

        /**
         * @brief 获取阶段系统数量
         * @param stage 阶段
         * @return 系统数量
         */
        size_t GetSystemCount(EStage stage) const;

    private:
        struct SystemEntry
        {
            std::string name;
            SystemFunc  func;
        };

        std::array<std::vector<SystemEntry>, static_cast<size_t>(EStage::Count)> _stages;
    };
} // namespace MMO