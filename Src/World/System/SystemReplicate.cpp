#include "SystemReplicate.h"
#include "World/Component/Health.h"
#include "World/Component/PlayerConnection.h"
#include "World/Component/Position.h"
#include "World/Component/Tags.h"

#include "Replicate.pb.h"

#include <algorithm>
#include <atomic>
#include <thread>

namespace MMO
{

    namespace
    {
        /** @brief 单实体 Update 打包（含预算内位置/血量/标签） */
        void PackUpdateEntity(ECS::Scene &scene, entt::entity e, Proto::EntityReplicateNtf &ntf)
        {
            auto *update = ntf.add_updates();
            update->set_entity_id(static_cast<uint64>(scene.ToEntityID(e)));

            auto &reg = scene.GetEnttRegistry();
            if (auto *pos = reg.try_get<Position>(e))
            {
                auto *pd = update->mutable_position();
                pd->set_x(static_cast<int32>(pos->x * 100.0f));
                pd->set_y(static_cast<int32>(pos->y * 100.0f));
                pd->set_z(static_cast<int32>(pos->z * 100.0f));
            }
            if (auto *hp = reg.try_get<Health>(e))
            {
                update->set_hp_current(hp->current);
            }
            if (reg.all_of<DeadTag>(e))
            {
                update->set_is_dead(true);
            }
            if (reg.all_of<CombatTag>(e))
            {
                update->set_is_in_combat(true);
            }
        }

        /** @brief 单实体 Spawn 打包（含完整实体信息） */
        void PackSpawnEntity(ECS::Scene &scene, entt::entity e, Proto::EntityReplicateNtf &ntf)
        {
            auto *spawn = ntf.add_spawns();
            spawn->set_entity_id(static_cast<uint64>(scene.ToEntityID(e)));

            auto &reg = scene.GetEnttRegistry();
            if (auto *pos = reg.try_get<Position>(e))
            {
                auto *pd = spawn->mutable_position();
                pd->set_x(static_cast<int32>(pos->x * 100.0f));
                pd->set_y(static_cast<int32>(pos->y * 100.0f));
                pd->set_z(static_cast<int32>(pos->z * 100.0f));
            }
            if (auto *hp = reg.try_get<Health>(e))
            {
                spawn->set_hp_current(hp->current);
                spawn->set_hp_max(hp->max);
            }
            if (reg.all_of<MonsterTag>(e))
            {
                spawn->set_entity_type(static_cast<uint32>(ECS::EEntityType::Monster));
            }
            else if (reg.all_of<PlayerTag>(e))
            {
                spawn->set_entity_type(static_cast<uint32>(ECS::EEntityType::Player));
            }
        }

        /** @brief 单实体 Despawn 打包 */
        void PackDespawnEntity(ECS::Scene &scene, entt::entity e, Proto::EntityReplicateNtf &ntf)
        {
            ntf.add_despawns()->set_entity_id(static_cast<uint64>(scene.ToEntityID(e)));
        }
    } // namespace

    std::vector<uint8>
    PackPlayerReplicate(ECS::Scene                                                       &scene,
                        ECS::EntityIndex                                                  observerIdx,
                        const std::unordered_set<ECS::EntityIndex>                       &visible,
                        const std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>> &enters,
                        const std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>> &leaves)
    {
        Proto::EntityReplicateNtf ntf;

        // ── 1. 本玩家相关的 leave → Despawn ──
        for (auto &[obs, ent] : leaves)
        {
            if (obs == observerIdx)
            {
                PackDespawnEntity(scene, entt::entity(static_cast<entt::id_type>(ent.raw)), ntf);
            }
        }

        // ── 2. 本玩家相关的 enter → Spawn ──
        for (auto &[obs, ent] : enters)
        {
            if (obs == observerIdx)
            {
                PackSpawnEntity(scene, entt::entity(static_cast<entt::id_type>(ent.raw)), ntf);
            }
        }

        // ── 3. 可见实体 → Update（含玩家自己——登录时单独 spawn，这里全量位置）──
        for (ECS::EntityIndex entIdx : visible)
        {
            PackUpdateEntity(scene, entt::entity(static_cast<entt::id_type>(entIdx.raw)), ntf);
        }

        if (ntf.spawns_size() == 0 && ntf.updates_size() == 0 && ntf.despawns_size() == 0)
        {
            return {};
        }

        const size_t                sz = static_cast<size_t>(ntf.ByteSizeLong());
        std::vector<uint8>          bytes(sz);
        [[maybe_unused]] const bool ok = ntf.SerializeToArray(bytes.data(), static_cast<int>(sz));
        (void)ok;
        return bytes;
    }

    void ReplicateScheduler::Update(
        const std::unordered_map<ECS::EntityIndex, std::unordered_set<ECS::EntityIndex>> &aoiState,
        const std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>>                 &enters,
        const std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>>                 &leaves)
    {
        if (aoiState.empty())
        {
            return;
        }

        // ── 并行打包：每玩家一个任务，结果按任务槽位独占写（无竞态）──
        struct TaskResult
        {
            uint32             sessionID;
            std::vector<uint8> bytes;
        };

        std::vector<ECS::EntityIndex> observers;
        observers.reserve(aoiState.size());
        for (auto &[obsIdx, _] : aoiState)
        {
            observers.push_back(obsIdx);
        }

        const size_t            taskCount = observers.size();
        std::vector<TaskResult> results(taskCount); // 预分配槽位——任务 i 独占写 results[i]
        std::atomic<size_t>     nextTask {0};

        std::vector<std::thread> workers;
        const size_t             wc = std::min(_workerCount, taskCount);
        for (size_t w = 0; w < wc; ++w)
        {
            workers.emplace_back([&]() {
                while (true)
                {
                    const size_t i = nextTask.fetch_add(1, std::memory_order_relaxed);
                    if (i >= taskCount)
                    {
                        break;
                    }
                    const auto obsIdx = observers[i];
                    auto       it     = aoiState.find(obsIdx);
                    if (it == aoiState.end())
                    {
                        continue;
                    }
                    auto bytes = PackPlayerReplicate(_scene, obsIdx, it->second, enters, leaves);
                    if (bytes.empty())
                    {
                        continue;
                    }
                    // 找 sessionID（只读 try_get，安全）
                    auto e = entt::entity(static_cast<entt::id_type>(obsIdx.raw));
                    if (auto *conn = _scene.GetEnttRegistry().try_get<PlayerConnection>(e))
                    {
                        results[i] = TaskResult {conn->sessionID, std::move(bytes)}; // 槽位独占写
                    }
                }
            });
        }
        for (auto &t : workers)
        {
            t.join();
        }

        // ── 串行发送（发送线程安全由 IO 层保证）──
        for (auto &r : results)
        {
            if (!r.bytes.empty())
            {
                _sendFunc(r.sessionID, r.bytes.data(), r.bytes.size());
            }
        }
    }
} // namespace MMO