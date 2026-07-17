/**
 * @file System.h
 * @brief CPPSystems 入口——脚本 Tick 后运行的 C++ EnTT 系统
 *
 * 所有 CPPSystems 在 LogicThread 中顺序执行，操作 EnTT Component。
 * 当前 2 个系统：MovementSystem (Position += Velocity × dt)、
 *            CombatTimeoutSystem (5s 无战斗 → 退出战斗状态)。
 */
#pragma once

namespace MMO::ECS
{
    class Scene;
} // namespace MMO::ECS

namespace MMO
{

    /**
     * @brief Position += Velocity × dt
     *
     * 遍历所有有 Position + Velocity 同时不含 DeadTag 的 entity。
     * 最简单的线性位移——Phase 3 MVP。
     */
    void SystemMovement(ECS::Scene &scene, float dt);

    /**
     * @brief CombatTag 超时自动移除
     *
     * 遍历所有有 CombatTag + LastCombatTime 的 entity，
     * 若距上次战斗 > 5s 则移除 CombatTag。
     */
    void SystemCombatTimeout(ECS::Scene &scene, float now);

    /**
     * @brief 对指定场景运行所有 CPPSystems
     *
     * 在 OnTick 中脚本 Update() 之后调用。
     */
    void RunCPPSystems(ECS::Scene &scene, float dt);

} // namespace MMO
