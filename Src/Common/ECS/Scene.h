/**
 * @file Scene.h
 * @brief 场景——EnTT registry + 脚本组件存储的容器
 *
 * 每个 Scene 持有独立 EnTT registry（C++ 高频组件）
 * 和 ScriptComponentStorage 集合（脚本层组件）。
 */
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>

#include "Common/Core/Types.h"
#include "Common/ECS/Entity.h"

namespace MMO::ECS
{

    /**
     * @brief 场景——EnTT registry + 脚本组件存储的容器
     *
     * 每个场景独立一个 Scene 实例。
     * C++ 高频组件（Position/Velocity/Health）走 EnTT registry 的 SoA 存储。
     * 脚本组件走 ScriptComponentStorage 的 SoA Blob 列。
     */
    class Scene
    {
    public:
        explicit Scene(uint32 sceneId);

        Scene(const Scene &)            = delete;
        Scene &operator=(const Scene &) = delete;
        Scene(Scene &&)                 = delete;
        Scene &operator=(Scene &&)      = delete;

        ~Scene() = default;

        uint32 SceneID() const
        {
            return _sceneId;
        }

        // ── Entity 生命周期 ──

        /**
         * @brief 创建实体
         * @return Entity{sceneId, entityId}
         */
        Entity CreateEntity();

        /**
         * @brief 销毁实体（同时清理脚本组件）
         * @param entity  Entity
         */
        void DestroyEntity(const Entity &entity);

        /**
         * @brief 实体是否有效
         * @param entity  Entity
         */
        bool IsValid(const Entity &entity) const;

        // ── C++ 组件（EnTT 存储）──

        /**
         * @brief 获取 entity 的组件引用
         * @tparam T  组件类型
         * @return T&
         */
        template <typename T>
        T &GetComponent(const Entity &entity)
        {
            return _registry.get<T>(entt::entity(entity.entityId));
        }

        template <typename T>
        const T &GetComponent(const Entity &entity) const
        {
            return _registry.get<T>(entt::entity(entity.entityId));
        }

        template <typename T>
        bool HasComponent(const Entity &entity) const
        {
            return _registry.all_of<T>(entt::entity(entity.entityId));
        }

        template <typename T, typename... Args>
        T &EmplaceComponent(const Entity &entity, Args &&...args)
        {
            return _registry.emplace<T>(entt::entity(entity.entityId), std::forward<Args>(args)...);
        }

        template <typename T>
        void RemoveComponent(const Entity &entity)
        {
            _registry.remove<T>(entt::entity(entity.entityId));
        }

        // ── 批量操作（ecs_query 底层）──

        template <typename... Components>
        auto CreateView()
        {
            return _registry.view<Components...>();
        }

        template <typename... Owned>
        auto CreateGroup()
        {
            return _registry.group<entt::owned_t<Owned...>>();
        }

        entt::registry &Registry()
        {
            return _registry;
        }

        const entt::registry &Registry() const
        {
            return _registry;
        }

        // ── 脚本组件存储 ──

        /**
         * @brief 注册一个脚本组件类型
         * @param name            组件名
         * @param componentSize   单组件字节数
         */
        void RegisterScriptComponent(const std::string &name, size_t componentSize);

    private:
        uint32         _sceneId;
        entt::registry _registry;
    };

} // namespace MMO::ECS
