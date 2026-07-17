/**
 * @file EntityType.h
 * @brief 实体类型枚举
 *
 * 统一 EnTT（create_entity entityType 参数）和 Protobuf（EntitySpawnNtf.entity_type 字段）。
 * 新增类型只追加到末尾，已有值永不改变。
 */
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 实体类型——EnTT + Protobuf 共用
     */
    enum class EEntityType : int32
    {
        ENTITY_PLAYER  = 1,
        ENTITY_NPC     = 2,
        ENTITY_MONSTER = 3,
    };

} // namespace MMO
