/**
 * @file Velocity.h
 * @brief 实体移动速度 Component
 *
 * 物理模拟专属——Movement System 根据 MoveIntent 更新 Velocity，
 * 再由 PhysicsSystem 应用 Position += Velocity * dt。
 * 脚本只读不写。
 */
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 3D 移动速度 (units/s)
     *
     * Swap_and_pop 删除——高频瞬态组件。
     */
    struct Velocity
    {
        float vx = 0.0f;
        float vy = 0.0f;
        float vz = 0.0f;
    };

} // namespace MMO
