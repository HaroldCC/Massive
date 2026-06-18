/**
 * @file Scene.cpp
 * @brief 场景实现
 */

#include "Common/ECS/Scene.h"

namespace MMO::ECS
{

Scene::Scene(uint32 sceneId)
    : _sceneId(sceneId)
{
}

Entity Scene::CreateEntity()
{
    auto e = _registry.create();
    return Entity{_sceneId, static_cast<uint32>(e)};
}

void Scene::DestroyEntity(const Entity& entity)
{
    for (auto& [name, storage] : _scriptStorages)
    {
        storage->Remove(entity.entityId);
    }
    _registry.destroy(entt::entity(entity.entityId));
}

bool Scene::IsValid(const Entity& entity) const
{
    if (entity.sceneId != _sceneId)
    {
        return false;
    }
    return _registry.valid(entt::entity(entity.entityId));
}

void Scene::RegisterScriptComponent(const std::string& name, size_t componentSize)
{
    auto storage = std::make_unique<ScriptComponentStorage>(name, componentSize);
    _scriptStorages[name] = std::move(storage);
}

ScriptComponentStorage* Scene::GetScriptStorage(const std::string& name)
{
    auto it = _scriptStorages.find(name);
    if (it == _scriptStorages.end())
    {
        return nullptr;
    }
    return it->second.get();
}

} // namespace MMO::ECS
