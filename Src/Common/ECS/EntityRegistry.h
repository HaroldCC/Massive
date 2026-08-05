#pragma once

#include "Common/Core/Types.h"
#include "EntityID.h"
#include "entt/entt.hpp"

namespace MMO::ECS
{
    class EntityRegistry
    {
    public:
        EntityRegistry(uint16 sceneID);
        EntityID     Create();
        bool         Destroy(EntityID eid);
        bool         IsValid(EntityID eid) const;
        entt::entity Resolve(EntityID eid) const;
        EntityID     ToEntityID(entt::entity e) const;

        uint16 SceneID() const
        {
            return _sceneID;
        }

        uint32 AliveCount() const
        {
            return _aliveCount;
        }

    private:
        uint16                    _sceneID {0};
        std::vector<uint32>       _versions;
        std::vector<entt::entity> _enttByIndex;
        std::vector<uint32>       _freeList;
        uint32                    _aliveCount {0};
    };
} // namespace MMO::ECS