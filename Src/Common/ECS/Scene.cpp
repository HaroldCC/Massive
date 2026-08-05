#include "Scene.h"

namespace MMO::ECS
{
    Scene::Scene(uint16 sceneID) : _sceneID(sceneID), _registry(sceneID)
    {
    }

    EntityID Scene::CreateEntity()
    {
        // 双 registry 同步：EntityRegistry 分配身份（index/version），
        // EnTT registry 用同一 index 创建组件存储实体。
        const uint64 id  = _registry.Create();
        const uint32 idx = IndexOf(EntityID(id));
        // 用指定 index 创建 EnTT 实体（index 从 0 递增，与 EntityRegistry 对齐）
        [[maybe_unused]] auto enttEntity = _entt.create(entt::entity(static_cast<entt::id_type>(idx)));
        return EntityID(id);
    }

    bool Scene::DestroyEntity(EntityID eid)
    {
        auto e = _registry.Resolve(eid);
        if (e == entt::null)
        {
            return false;
        }

        // 从空间索引清理残留（否则 QueryRadius 返回幽灵 index，
        // 依赖调用方二次过滤掩盖——EntitiesInRadius 类 API 直接暴露）
        _grid.Remove(EntityIndex(static_cast<uint32>(entt::to_integral(e))));

        _entt.destroy(e);
        return _registry.Destroy(eid);
    }

    bool Scene::IsValidEntity(EntityID eid) const
    {
        return _registry.IsValid(eid);
    }

    entt::entity Scene::ResolveEntity(EntityID eid) const
    {
        return _registry.Resolve(eid);
    }

    EntityID Scene::ToEntityID(entt::entity e) const
    {
        return _registry.ToEntityID(e);
    }
} // namespace MMO::ECS