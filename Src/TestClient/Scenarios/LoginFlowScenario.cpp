/**
 * @file LoginFlowScenario.cpp
 * @brief Scenario 实现——LoginFlow / Idle / MoveStress + 工厂
 */
#include <algorithm>
#include <random>

#include "TestClient/Scenarios/LoginFlowScenario.h"
#include "TestClient/VirtualClient.h"
#include "Common/Log/Log.h"

#include <MsgID.pb.h>
#include <Move.pb.h>

namespace MMO::TestClient
{

    /** @brief LoginFlowScenario */

    LoginFlowScenario::LoginFlowScenario(uint32 heartbeatIntervalSec,
                                        uint32 moveIntervalMs,
                                        float  moveSpeed,
                                        float  moveRadius,
                                        uint32 durationSec)
        : _heartbeatIntervalSec(heartbeatIntervalSec)
        , _heartbeatIntervalMs(heartbeatIntervalSec * 1000)
        , _moveIntervalMs(moveIntervalMs)
        , _moveSpeed(moveSpeed)
        , _moveRadius(moveRadius)
        , _durationSec(durationSec)
        , _rng(std::random_device {}())
    {
    }

    void LoginFlowScenario::OnEnter()
    {
        _enterTime         = std::chrono::steady_clock::now();
        _elapsedHeartbeatMs = 0;
        _elapsedMoveMs      = 0;

        Log::Info("[{}] LoginFlow started (hb={}s, move={}ms, speed={}, radius={})",
                  _owner ? _owner->Name() : "?",
                  _heartbeatIntervalSec, _moveIntervalMs, _moveSpeed, _moveRadius);
    }

    void LoginFlowScenario::OnTick(uint32 elapsedMs)
    {
        if (!_owner)
        {
            return;
        }

        // 检查总时长
        if (_durationSec > 0)
        {
            auto now  = std::chrono::steady_clock::now();
            auto dur  = std::chrono::duration_cast<std::chrono::seconds>(now - _enterTime).count();
            if (static_cast<uint32>(dur) >= _durationSec)
            {
                Log::Info("[{}] LoginFlow complete (duration={}s)", _owner->Name(), _durationSec);
                _owner->Disconnect();
                return;
            }
        }

        // ── 心跳 ──
        _elapsedHeartbeatMs += elapsedMs;
        if (_elapsedHeartbeatMs >= _heartbeatIntervalMs)
        {
            _elapsedHeartbeatMs -= _heartbeatIntervalMs;
            _owner->SendHeartbeat();
        }

        // ── 移动 ──
        if (_moveIntervalMs > 0)
        {
            _elapsedMoveMs += elapsedMs;
            if (_elapsedMoveMs >= _moveIntervalMs)
            {
                _elapsedMoveMs -= _moveIntervalMs;

                // 随机方向移动
                std::uniform_real_distribution<float> dirDist(-1.0f, 1.0f);
                std::uniform_real_distribution<float> stepDist(0.0f, 1.0f);

                float dx = dirDist(_rng);
                float dz = dirDist(_rng);
                float len = std::sqrt(dx * dx + dz * dz);
                if (len > 0.001f)
                {
                    dx /= len;
                    dz /= len;
                }

                float step = stepDist(_rng) * _moveSpeed * (_moveIntervalMs / 1000.0f);
                _x += dx * step;
                _z += dz * step;

                // 限制在半径内
                float dist = std::sqrt(_x * _x + _z * _z);
                if (dist > _moveRadius)
                {
                    _x *= _moveRadius / dist;
                    _z *= _moveRadius / dist;
                }

                _owner->SendMove(_moveSequence++, _x, _y, _z, _moveSpeed);
            }
        }
    }

    void LoginFlowScenario::OnMoveRsp([[maybe_unused]] uint32 sequence)
    {
        _moveRspCount++;
    }

    /** @brief IdleScenario -- 只发心跳 */

    IdleScenario::IdleScenario(uint32 heartbeatIntervalSec)
        : _heartbeatIntervalMs(heartbeatIntervalSec * 1000)
    {
    }

    void IdleScenario::OnEnter()
    {
        _enterTime  = std::chrono::steady_clock::now();
        _elapsedMs  = 0;
    }

    void IdleScenario::OnTick(uint32 elapsedMs)
    {
        if (!_owner)
        {
            return;
        }

        _elapsedMs += elapsedMs;
        if (_elapsedMs >= _heartbeatIntervalMs)
        {
            _elapsedMs -= _heartbeatIntervalMs;
            _owner->SendHeartbeat();
        }
    }

    /** @brief MoveStressScenario -- 密集 MoveReq */

    MoveStressScenario::MoveStressScenario(uint32 heartbeatIntervalSec,
                                          uint32 moveIntervalMs,
                                          float  moveSpeed,
                                          float  moveRadius)
        : _heartbeatIntervalMs(heartbeatIntervalSec * 1000)
        , _moveIntervalMs(moveIntervalMs)
        , _moveSpeed(moveSpeed)
        , _moveRadius(moveRadius)
        , _rng(std::random_device {}())
    {
    }

    void MoveStressScenario::OnEnter()
    {
        _elapsedHeartbeatMs = 0;
        _elapsedMoveMs      = 0;
    }

    void MoveStressScenario::OnTick(uint32 elapsedMs)
    {
        if (!_owner)
        {
            return;
        }

        _elapsedHeartbeatMs += elapsedMs;
        if (_elapsedHeartbeatMs >= _heartbeatIntervalMs)
        {
            _elapsedHeartbeatMs -= _heartbeatIntervalMs;
            _owner->SendHeartbeat();
        }

        _elapsedMoveMs += elapsedMs;
        if (_elapsedMoveMs >= _moveIntervalMs)
        {
            _elapsedMoveMs -= _moveIntervalMs;

            std::uniform_real_distribution<float> dirDist(-1.0f, 1.0f);
            float dx = dirDist(_rng);
            float dz = dirDist(_rng);
            float len = std::sqrt(dx * dx + dz * dz);
            if (len > 0.001f)
            {
                dx /= len;
                dz /= len;
            }

            float step = _moveSpeed * (_moveIntervalMs / 1000.0f);
            _x += dx * step;
            _z += dz * step;

            float dist = std::sqrt(_x * _x + _z * _z);
            if (dist > _moveRadius)
            {
                _x *= _moveRadius / dist;
                _z *= _moveRadius / dist;
            }

            _owner->SendMove(_moveSequence++, _x, _y, _z, _moveSpeed);
        }
    }

    void MoveStressScenario::OnMoveRsp([[maybe_unused]] uint32 sequence)
    {
        _moveRspCount++;
    }

    /** @brief 工厂 */

    std::unique_ptr<Scenario> CreateScenario([[maybe_unused]] const std::string &name)
    {
        // MVP: 所有场景由 VirtualClient 创建时指定参数，此处为占位
        return nullptr;
    }

} // namespace MMO::TestClient
