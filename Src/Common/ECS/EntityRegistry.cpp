#include "EntityRegistry.h"
#include "entt/core/fwd.hpp"
#include "entt/entity/entity.hpp"
#include "entt/entity/fwd.hpp"

namespace MMO::ECS
{

    EntityRegistry::EntityRegistry(uint16 sceneID) : _sceneID(sceneID)
    {
    }

    EntityID EntityRegistry::Create()
    {
        uint32       index = 0;
        entt::entity e;

        if (!_freeList.empty())
        {
            index = _freeList.back();
            _freeList.pop_back();
            e = _enttByIndex[index];

            const uint32 newVersion = _versions[index] + 1;
            _versions[index]        = (newVersion & kVersionMask) == 0 ? 1 : newVersion;
        }
        else
        {
            index = static_cast<uint32>(_versions.size());
            _versions.push_back(1);
            e = entt::entity(static_cast<entt::id_type>(index));
            _enttByIndex.push_back(e);
        }

        ++_aliveCount;
        return MakeEntityID(_sceneID, index, _versions[index]);
    }

    bool EntityRegistry::Destroy(EntityID eid)
    {
        if (!IsValid(eid))
        {
            return false;
        }

        const uint32 index = IndexOf(eid);
        // 版本回绕到 0 时跳 1（与 Create 的防护一致）——0 表示"该 index 从未分配/已失效"，
        // 若回绕到 0，IsValid 的 `_versions[index] != 0` 会把存活的实体误判为无效
        const uint32 newVersion = (_versions[index] + 1) & kVersionMask;
        _versions[index]        = newVersion == 0 ? 1 : newVersion;
        _freeList.push_back(index);
        --_aliveCount;
        return true;
    }

    bool EntityRegistry::IsValid(EntityID eid) const
    {
        if (!IsValidEntity(eid) || SceneOf(eid) != _sceneID)
        {
            return false;
        }

        const uint32 index = IndexOf(eid);
        if (index >= _versions.size())
        {
            return false;
        }

        return _versions[index] != 0 && _versions[index] == VersionOf(eid);
    }

    entt::entity EntityRegistry::Resolve(EntityID eid) const
    {
        if (!IsValid(eid))
        {
            return entt::null;
        }

        return _enttByIndex[IndexOf(eid)];
    }

    EntityID EntityRegistry::ToEntityID(entt::entity e) const
    {
        const uint32 index = static_cast<uint32>(entt::to_integral(e));
        if (index >= _versions.size() || _versions[index] == 0)
        {
            return kInvalidEntityID;
        }

        return MakeEntityID(_sceneID, index, _versions[index]);
    }
} // namespace MMO::ECS