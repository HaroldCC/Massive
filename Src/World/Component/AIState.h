#pragma once

#include "Common/Core/Types.h"
#include "Common/ECS/EntityID.h"

namespace MMO
{

    /**
     * @brief 怪物 AI 状态（低频决策，脚本 [game_system] 读写）
     *
     * 状态机：IDLE → PATROL → COMBAT → FLEE
     * 脚本读取/写入均经 Bridge（写回 MarkDirty）。
     */
    enum class EAIState : int32
    {
        IDLE   = 0,
        PATROL = 1,
        COMBAT = 2,
        FLEE   = 3,
    };

    struct AIState
    {
        EAIState      state     = EAIState::IDLE;
        ECS::EntityID targetID  = 0;    // 当前目标实体
        float         stateTime = 0.0f; // 当前状态持续时长
    };

} // namespace MMO
