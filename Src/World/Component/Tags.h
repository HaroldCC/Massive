#pragma once

namespace MMO
{

    /**
     * @brief 实体已死亡（currentHp == 0)
     */
    struct DeadTag
    {
    };

    /** @brief 实体处于战斗状态 */
    struct CombatTag
    {
    };

    /** @brief 实体被眩晕/定身 */
    struct StunnedTag
    {
    };

    /** @brief 实体为玩家 */
    struct PlayerTag
    {
    };

    /** @brief 实体为怪物/NPC */
    struct MonsterTag
    {
    };

    /** @brief 实体休眠（AOI 外，跳过模拟） */
    struct DormantTag
    {
    };

} // namespace MMO