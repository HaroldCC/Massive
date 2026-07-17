/**
 * @file Health.h
 * @brief 实体生命值 Component
 *
 * 仅跟踪 current/max 数值，不包含战斗属性（见 BattleStats.h）。
 * Swap_and_pop 删除——高频瞬态组件。
 */
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 生命值
     *
     * current ∈ [0, max]。由 Combat System 写入，脚本可读可写。
     */
    struct Health
    {
        int32 current = 0;
        int32 max     = 0;
    };

} // namespace MMO
