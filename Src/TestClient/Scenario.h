/**
 * @file Scenario.h
 * @brief 虚拟客户端行为接口
 *
 * VirtualClient 在进入 InWorld 状态后，由 Scenario 驱动其后续行为。
 * 每个 Scenario 是一个回调驱动的状态机片段，由 VirtualClient 在收到响应时触发。
 */
#pragma once

#include "Common/Core/Types.h"

#include <memory>
#include <string>

namespace MMO::TestClient
{

    class VirtualClient;

    /**
     * @brief 客户端行为场景基类
     *
     * 生命周期：
     *   1. 构造 → Start() → 行为循环
     *   2. OnTick() 定期回调（20ms tick）
     *   3. Stop() 或析构结束
     *
     * 派生类覆写 OnEnter / OnTick / OnMoveRsp 等方法。
     */
    class Scenario
    {
    public:
        Scenario()          = default;
        virtual ~Scenario() = default;

        Scenario(const Scenario &)            = delete;
        Scenario &operator=(const Scenario &) = delete;

        /**
         * @brief 场景名称（用于日志和统计）
         */
        virtual const char *Name() const = 0;

        /**
         * @brief 进入场景（从 Idle 转换时调用一次）
         */
        virtual void OnEnter()
        {
        }

        /**
         * @brief 每 20ms 逻辑 Tick
         * @param elapsedMs  距上次 Tick 的毫秒数
         */
        virtual void OnTick(uint32 elapsedMs)
        {
        }

        /**
         * @brief 收到 MoveRsp 消息
         * @param sequence 请求序号
         */
        virtual void OnMoveRsp(uint32 sequence)
        {
        }

        /**
         * @brief 场景结束，准备切换到其他场景
         */
        virtual void OnExit()
        {
        }

        /**
         * @brief 场景内部需要被外部定时器驱动
         * 返回 true 表示需要 OnTick
         */
        virtual bool NeedsTick() const
        {
            return true;
        }

    protected:
        VirtualClient *_owner = nullptr;
        friend class VirtualClient;
    };

    /**
     * @brief 根据名称创建场景实例的工厂
     * @param name  场景名称
     * @return Scenario*，不识别返回 nullptr
     */
    std::unique_ptr<Scenario> CreateScenario([[maybe_unused]] const std::string &name);

} // namespace MMO::TestClient
