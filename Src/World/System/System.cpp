/**
 * @file System.cpp
 * @brief CPPSystems 实现——MovementSystem + AOISystem
 *
 * 执行顺序（RunCPPSystems）:
 *   1. MovementSystem: Position += Velocity × dt
 *   2. AOISystem:      全量 O(N×M) 计算每个 player 的可见 entity 集合
 */

#include "World/System/System.h"

#include <cmath>

#include "Common/ECS/Scene.h"
#include "Common/Log/Log.h"
#include "World/Component/Position.h"
#include "World/Component/Tags.h"
#include "World/Component/Velocity.h"

namespace MMO
{

    // ═══════════════════════════════════════════════════════════════
    // Phase 3: MovementSystem — Position += Velocity × dt
    // ═══════════════════════════════════════════════════════════════

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

    // ═══════════════════════════════════════════════════════════════
    // Phase 4: AOISystem — 全量遍历空间索引
    // ═══════════════════════════════════════════════════════════════

    void SystemAOI(ECS::Scene &scene,
                   std::unordered_map<uint32_t, VisibleSet> &outVisibleSets)
    {
        auto &reg = scene.Registry();

        // ── Step 1: 采集所有有 Position 的 entity ──
        struct EntityEntry
        {
            uint32_t id;
            float    x, y, z;
        };
        std::vector<EntityEntry> entities;
        {
            auto view = reg.view<const Position>();
            entities.reserve(view.size());
            for (auto [e, pos] : view.each())
            {
                entities.push_back(
                    {static_cast<uint32_t>(entt::to_integral(e)), pos.x, pos.y, pos.z});
            }
        }

        // ── Step 2: 对每个 player 计算视野 ──
        {
            auto players = reg.view<const Position, const PlayerTag>();

            for (auto [e, pos] : players.each())
            {
                uint32_t    pid = static_cast<uint32_t>(entt::to_integral(e));
                VisibleSet &vs  = outVisibleSets[pid];
                vs.viewRadiusXZ = 100.0f;
                vs.viewRadiusY  = 15.0f;
                vs.entityIDs.reserve(entities.size());

                const float rXzSq = vs.viewRadiusXZ * vs.viewRadiusXZ;

                for (auto &entry : entities)
                {
                    if (entry.id == pid)
                    {
                        continue;
                    }

                    float dx = entry.x - pos.x;
                    float dz = entry.z - pos.z;
                    float dy = entry.y - pos.y;

                    if (dx * dx + dz * dz <= rXzSq && std::abs(dy) <= vs.viewRadiusY)
                    {
                        vs.entityIDs.push_back(entry.id);
                    }
                }
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════
    // 调度入口
    // ═══════════════════════════════════════════════════════════════

    void RunCPPSystems(ECS::Scene &scene, float dt)
    {
        SystemMovement(scene, dt);

        // AOI 结果由 WorldServer 持有（Phase 4+ 供 ReplicateSystem 消费）
        // Phase 4 MVP: 仅计算并丢弃——验证 AOI 环路
        std::unordered_map<uint32_t, VisibleSet> visibleSets;
        SystemAOI(scene, visibleSets);
    }

} // namespace MMO
