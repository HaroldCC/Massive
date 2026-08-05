#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 玩家连接归属——实体 ↔ sessionID 双向映射的一侧
     *
     * 挂在玩家实体上，复制系统/离线处理通过它找 session。
     * 另一侧是 WorldSession::entityID（ECS_01 已加）。
     */
    struct PlayerConnection
    {
        uint32 sessionID    = 0;
        uint16 gateServerID = 0;
    };

} // namespace MMO