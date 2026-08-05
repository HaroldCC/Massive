#include "SystemMovement.h"
#include "World/Component/Position.h"
#include "World/Component/Velocity.h"
#include "World/Component/Tags.h"

namespace MMO
{

    /**
     * @brief 移动系统
     * @param registry entt registry
     * @param dt 固定步长
     */
    void SystemMovement(entt::registry &registry, float dt)
    {
        auto view = registry.view<Position, Velocity>(entt::exclude<DeadTag, DormantTag>);
        for (auto [e, pos, vel] : view.each())
        {
            pos.x += vel.vx * dt;
            pos.y += vel.vy * dt;
            pos.z += vel.vz * dt;
        }
    }
} // namespace MMO