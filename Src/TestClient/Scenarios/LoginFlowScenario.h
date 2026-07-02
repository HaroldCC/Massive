/**
 * @file LoginFlowScenario.h
 * @brief 完整登录流程场景——Login → EnterWorld → Heartbeat → Move（含 MoveHandler 加压测试）
 */
#pragma once

#include "TestClient/Scenario.h"

#include <chrono>
#include <random>

namespace MMO::TestClient
{

    /**
     * @brief 完整登录流程测试场景
     *
     * 状态转换：
     *   OnEnter → 发 Heartbeat 循环 + MoveReq 循环
     *
     * 行为：
     *   - 每 heartbeatIntervalSec 发一次 HeartbeatReq（对应 Gate 自回心跳）
     *   - 每 moveIntervalMs 发一次 MoveReq（服务器权威回包）
     *   - 走到 durationSecs 后 Stop → 断开
     */
    class LoginFlowScenario : public Scenario
    {
    public:
        LoginFlowScenario(uint32 heartbeatIntervalSec,
                          uint32 moveIntervalMs,
                          float  moveSpeed,
                          float  moveRadius,
                          uint32 durationSec);

        const char *Name() const override
        {
            return "LoginFlow";
        }

        void OnEnter() override;
        void OnTick(uint32 elapsedMs) override;
        void OnMoveRsp([[maybe_unused]] uint32 sequence) override;

    private:
        uint32 _heartbeatIntervalSec; // 心跳间隔（秒）
        uint32 _heartbeatIntervalMs;  // 同上（毫秒）
        uint32 _moveIntervalMs;       // 移动间隔（毫秒）
        float  _moveSpeed;
        float  _moveRadius;
        uint32 _durationSec;

        // 运行时状态
        std::chrono::steady_clock::time_point _enterTime;
        uint64                                _elapsedHeartbeatMs = 0;
        uint64                                _elapsedMoveMs      = 0;
        uint32                                _moveSequence       = 0;
        uint32                                _moveRspCount       = 0;
        float                                 _x = 0.0f, _y = 0.0f, _z = 0.0f;

        std::mt19937 _rng;
    };

    /**
     * @brief 挂机场景——仅心跳，不发 Move
     */
    class IdleScenario : public Scenario
    {
    public:
        explicit IdleScenario(uint32 heartbeatIntervalSec);

        const char *Name() const override
        {
            return "Idle";
        }

        void OnEnter() override;
        void OnTick(uint32 elapsedMs) override;

    private:
        uint32                                _heartbeatIntervalMs;
        uint64                                _elapsedMs = 0;
        std::chrono::steady_clock::time_point _enterTime;
    };

    /**
     * @brief 移动压力场景——密集 MoveReq
     */
    class MoveStressScenario : public Scenario
    {
    public:
        MoveStressScenario(uint32 heartbeatIntervalSec,
                           uint32 moveIntervalMs,
                           float  moveSpeed,
                           float  moveRadius);

        const char *Name() const override
        {
            return "MoveStress";
        }

        void OnEnter() override;
        void OnTick(uint32 elapsedMs) override;
        void OnMoveRsp([[maybe_unused]] uint32 sequence) override;

    private:
        uint32 _heartbeatIntervalMs;
        uint32 _moveIntervalMs;
        float  _moveSpeed;
        float  _moveRadius;

        uint64 _elapsedHeartbeatMs = 0;
        uint64 _elapsedMoveMs      = 0;
        uint32 _moveSequence       = 0;
        uint32 _moveRspCount       = 0;
        float  _x = 0.0f, _y = 0.0f, _z = 0.0f;

        std::mt19937 _rng;
    };

} // namespace MMO::TestClient
