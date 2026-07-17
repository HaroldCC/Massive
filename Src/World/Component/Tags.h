/**
 * @file Tags.h
 * @brief 实体状态 Tag Component（零存储零开销纯标记）
 *
 * 每个 Tag 各 1 个空 struct（size=0），EnTT tag 不占存储空间。
 * 脚本通过 massive_entity_is_* 函数查询。
 */
#pragma once

namespace MMO
{

    /**
     * @brief 实体已死亡（currentHp == 0）
     */
    struct DeadTag
    {
    };

    /**
     * @brief 实体处于战斗状态
     */
    struct CombatTag
    {
    };

    /**
     * @brief 实体被眩晕/定身
     */
    struct StunnedTag
    {
    };

    /**
     * @brief 实体为玩家
     */
    struct PlayerTag
    {
    };

    /**
     * @brief 实体为怪物/NPC
     */
    struct MonsterTag
    {
    };

} // namespace MMO
