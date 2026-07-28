/**
 * @file Scene.cpp
 * @brief 场景实现
 */

#include "Common/ECS/Scene.h"

namespace MMO::ECS
{

    Scene::Scene(uint32 sceneId) : _sceneId(sceneId)
    {
    }

    Entity Scene::CreateEntity()
    {
        auto e = _registry.create();
        return Entity {_sceneId, static_cast<uint32>(e)};
    }

    void Scene::DestroyEntity(const Entity &entity)
    {
        _registry.destroy(entt::entity(entity.entityId));
    }

    bool Scene::IsValid(const Entity &entity) const
    {
        if (entity.sceneId != _sceneId)
        {
            return false;
        }
        return _registry.valid(entt::entity(entity.entityId));
    }

} // namespace MMO::ECS
