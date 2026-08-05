#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 战斗属性（脚本经 Bridge 读写的完整属性集）
     *
     * 值语义：脚本拿快照，改完经 Bridge 写回（写回触发 MarkDirty）。
     * 后续可拆分为 base + buff 两层，MVP 先单层。
     */
    struct BattleStats
    {
        int32 attack       = 0; // 物理攻击
        int32 defense      = 0; // 物理防御
        int32 magicAttack  = 0; // 魔法攻击
        int32 magicDefense = 0; // 魔法防御
        int32 critRate     = 0; // 暴击率（万分比）
        int32 critDamage   = 0; // 暴击伤害倍率（万分比）
        int32 dodgeRate    = 0; // 闪避率（万分比）
        int32 hitRate      = 0; // 命中率（万分比）
        int32 attackSpeed  = 0; // 攻击速度（万分比）
        int32 moveSpeed    = 0; // 移动速度（万分比）
        int32 maxHp        = 0; // 最大生命值
        int32 maxMp        = 0; // 最大魔法值
    };

} // namespace MMO