/**
 * @file System.cpp
 * @brief CPPSystems 实现——MovementSystem + CombatTimeoutSystem
 */

#include "World/System/System.h"

#include "Common/ECS/Scene.h"
#include "Common/Log/Log.h"
#include "World/Component/Position.h"
#include "World/Component/Tags.h"
#include "World/Component/Velocity.h"

namespace MMO
{

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

    void SystemCombatTimeout(ECS::Scene &scene, float now)
    {
        auto view = scene.Registry().view<CombatTag>();

        // 遍历所有战斗中的 entity，通过 timing 判断超时
        // Phase 3 简化：无 LastCombatTime 组件 → 直接用系统时间做超时
        // 实际超时检查由脚本层通过 ScheduleTimer 管理

        (void)scene;
        (void)now;
        // 超时逻辑由脚本层在 DECS 中驱动（ScheduleTimer 回调）
        // C++ 侧仅保留 MovementSystem（帧同步硬实时）
    }

    void RunCPPSystems(ECS::Scene &scene, float dt)
    {
        SystemMovement(scene, dt);

        // CombatTimeout 由脚本层管理——Phase 3 不运行
    }

} // namespace MMO
