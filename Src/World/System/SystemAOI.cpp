#include "SystemAOI.h"
#include "World/Component/Position.h"
#include "World/Component/Velocity.h"
#include "World/Component/PlayerConnection.h"

namespace MMO
{
    void SystemAOI(entt::registry                                                             &reg,
                   ECS::Grid                                                                  &grid,
                   std::unordered_map<ECS::EntityIndex, std::unordered_set<ECS::EntityIndex>> &prevState,
                   std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>>                 &outEnter,
                   std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>>                 &outLeave)
    {
        constexpr float kViewRadiusXZ = 100.0f;
        constexpr float kViewRadiusY  = 15.0f;

        outEnter.clear();
        outLeave.clear();

        // ── 1. 更新格子：所有有 Position 的实体（含玩家——玩家无 Velocity，
        //    不能只盯 Position+Velocity，否则静止玩家/脚本新建实体永不在格）──
        //    Grid.Update 对未插入实体自动 Insert（Grid.cpp 自愈），
        //    全量 view 每帧保证：新实体首次进格、移动实体跨格更新、静止实体保持。
        auto movers = reg.view<Position>();
        for (auto [e, pos] : movers.each())
        {
            const ECS::EntityIndex idx(static_cast<uint32>(entt::to_integral(e)));
            grid.Update(idx, pos.x, pos.z);
        }

        // ── 2. 每个玩家计算可见集 ──
        auto players = reg.view<Position, PlayerConnection>();
        for (auto [pe, ppos, pconn] : players.each())
        {
            const ECS::EntityIndex observerIdx(static_cast<uint32>(entt::to_integral(pe)));
            const float            px = ppos.x, py = ppos.y, pz = ppos.z;

            // 候选（格子粗筛）
            std::vector<ECS::EntityIndex> candidates;
            grid.QueryRadius(px, pz, kViewRadiusXZ, candidates);

            // 精确过滤（XZ 距离 + Y 高度）
            std::unordered_set<ECS::EntityIndex> current;
            const float                          rXzSq = kViewRadiusXZ * kViewRadiusXZ;
            for (ECS::EntityIndex idx : candidates)
            {
                if (idx == observerIdx)
                {
                    continue;
                }
                auto entity = entt::entity(static_cast<entt::id_type>(idx.raw));
                if (!reg.all_of<Position>(entity))
                {
                    continue;
                }
                auto       &epos = reg.get<Position>(entity);
                const float dx   = epos.x - px;
                const float dz   = epos.z - pz;
                const float dy   = epos.y - py;
                if (dx * dx + dz * dz <= rXzSq && std::abs(dy) <= kViewRadiusY)
                {
                    current.insert(idx);
                }
            }

            // ── 3. 增量 diff ──
            auto &prev = prevState[observerIdx];
            for (ECS::EntityIndex idx : current)
            {
                if (!prev.contains(idx))
                {
                    outEnter.emplace_back(observerIdx, idx);
                }
            }

            for (ECS::EntityIndex idx : prev)
            {
                if (!current.contains(idx))
                {
                    outLeave.emplace_back(observerIdx, idx);
                }
            }
            prev = std::move(current);
        }
    }
} // namespace MMO