/**
 * @file System.h
 * @brief CPPSystems 入口——脚本 Tick 后运行的 C++ EnTT 系统
 *
 * 所有 CPPSystems 在 LogicThread 中顺序执行，操作 EnTT Component。
 * Phase 3: MovementSystem
 * Phase 4: + AOISystem（全量遍历空间索引）
 * Phase 5: + ReplicateSystem（网络复制，在 WorldServer 中实现）
 */
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMO::ECS
{
    class Scene;
} // namespace MMO::ECS

namespace MMO
{

    // ── Phase 3: 物理模拟 ──

    /**
     * @brief Position += Velocity × dt
     * @param scene 目标场景
     * @param dt    帧间隔（固定 0.02f）
     */
    void SystemMovement(ECS::Scene &scene, float dt);

    // ── Phase 4: 空间索引 ──

    /**
     * @brief 每个 player 的可见 entity 集合
     *
     * 由 AOISystem 每帧重新计算，被 ReplicateSystem 消费。
     */
    struct VisibleSet
    {
        std::vector<uint32_t> entityIDs;
        float viewRadiusXZ = 100.0f;
        float viewRadiusY  = 15.0f;
    };

    /**
     * @brief AOI 空间查询——全量遍历 O(N×M)
     *
     * 对每个有 Position + PlayerTag 的 entity 计算其视野内的所有实体。
     * Phase 4 MVP: 100 entity × 10 player 量级 < 0.01ms。
     * Phase 4+: 替换为网格/十字链表等空间索引。
     *
     * @param scene         目标场景
     * @param outVisibleSets  输出：playerEID → VisibleSet
     */
    void SystemAOI(ECS::Scene &scene,
                   std::unordered_map<uint32_t, VisibleSet> &outVisibleSets);

    // ── 调度入口 ──

    /**
     * @brief 对指定场景运行所有 CPPSystems
     *
     * 执行顺序：MovementSystem → AOISystem
     * 在 OnTick 中脚本 Update() 之后调用。
     *
     * @param scene         目标场景
     * @param dt            帧间隔
     * @param outVisibleSets  [输出] AOI 计算结果，供 ReplicateSystem 复用
     */
    void RunCPPSystems(ECS::Scene &scene, float dt,
                       std::unordered_map<uint32_t, VisibleSet> &outVisibleSets);

} // namespace MMO
