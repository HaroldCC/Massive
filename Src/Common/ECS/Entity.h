/**
 * @file Entity.h
 * @brief 跨场景实体标识符
 *
 * Entity { sceneId, entityId } 唯一标识世界中的一个实体。
 */
#pragma once

#include <cstdint>
#include <functional>

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 跨场景实体标识符
     *
     * (sceneId, entityId) 唯一定位一个实体。
     * sceneId 指向 WorldServer 管理的某个 Scene，
     * entityId 是 EnTT registry 中的 entt::entity 的 uint32 表示。
     */
    struct Entity
    {
        uint32 sceneId  = 0;          // Scene ID（0 为无效值）
        uint32 entityId = kInvalidID; // EnTT entity id

        bool operator==(const Entity &other) const = default;
        bool operator!=(const Entity &other) const = default;

        // 是否有效（sceneId != 0 且 entityId != kInvalidID）
        bool IsValid() const
        {
            return sceneId != 0 && entityId != kInvalidID;
        }
    };

    // 空 Entity 哨兵值
    inline constexpr Entity kInvalidEntity {0, kInvalidID};

} // namespace MMO

template <>
struct std::hash<MMO::Entity>
{
    size_t operator()(const MMO::Entity &e) const noexcept
    {
        return (static_cast<size_t>(e.sceneId) << 32) | e.entityId;
    }
};
