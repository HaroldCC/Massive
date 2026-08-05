#pragma once

#include "entt/entt.hpp"

namespace MMO
{
    /**
     * @brief 移动系统
     * @param registry entt registry
     * @param dt 固定步长
     */
    void SystemMovement(entt::registry &registry, float dt);
} // namespace MMO