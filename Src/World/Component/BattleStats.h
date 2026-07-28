/**
 * @file BattleStats.h
 * @brief 实体战斗属性 Component
 *
 * 整体暴露给脚本的只读属性集。
 * Phase 2: 基础攻防属性
 * Phase 3+: 新增暴击/穿透/抗性等字段
 */
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 战斗属性（一次调用全量获取，零字符串）
     */
    struct BattleStats
    {
        int32 attack       = 0; // 物理攻击
        int32 defense      = 0; // 物理防御
        int32 magicAttack  = 0; // 魔法攻击
        int32 magicDefense = 0; // 魔法防御
        int32 critRate     = 0; // 暴击率 (万分比)
        int32 critDamage   = 0; // 暴击伤害倍率 (万分比)
        int32 dodgeRate    = 0; // 闪避率 (万分比)
        int32 hitRate      = 0; // 命中率 (万分比)
        int32 attackSpeed  = 0; // 攻击速度 (万分比)
        int32 moveSpeed    = 0; // 移动速度 (万分比)
        int32 currentHp    = 0; // 当前生命值
        int32 maxHp        = 0; // 最大生命值
        int32 currentMp    = 0; // 当前魔法值
        int32 maxMp        = 0; // 最大魔法值
    };

} // namespace MMO
