#pragma once

#include "Common/Core/Types.h"
#include "Common/ECS/ActiveSet.h"
#include "Common/ECS/EntityID.h"
#include "Common/ECS/EntityRegistry.h"
#include "Common/ECS/DirtyIndex.h"
#include "Common/ECS/Grid.h"
#include "entt/entity/fwd.hpp"

namespace MMO::ECS
{
    class Scene
    {
    public:
        explicit Scene(uint16 sceneID);

        Scene(const Scene &)            = delete;
        Scene &operator=(const Scene &) = delete;
        Scene(Scene &&)                 = delete;
        Scene &operator=(Scene &&)      = delete;

        uint16 SceneID() const
        {
            return _sceneID;
        }

        EntityID     CreateEntity();
        bool         DestroyEntity(EntityID eid);
        bool         IsValidEntity(EntityID eid) const;
        entt::entity ResolveEntity(EntityID eid) const;
        EntityID     ToEntityID(entt::entity e) const;

        template <typename TCommponent>
        TCommponent &GetComponent(EntityID eid)
        {
            return _entt.get<TCommponent>(ResolveEntity(eid));
        }

        template <typename TCommponent>
        const TCommponent &GetComponent(EntityID eid) const
        {
            return _entt.get<TCommponent>(ResolveEntity(eid));
        }

        template <typename TCommponent>
        bool HasComponent(EntityID eid) const
        {
            return _entt.all_of<TCommponent>(ResolveEntity(eid));
        }

        template <typename TCommponent, typename... Args>
        TCommponent &EmplaceComponent(EntityID eid, Args &&...args)
        {
            return _entt.emplace<TCommponent>(ResolveEntity(eid), std::forward<Args>(args)...);
        }

        template <typename TCommponent>
        void RemoveComponent(EntityID eid)
        {
            _entt.remove<TCommponent>(ResolveEntity(eid));
        }

        template <typename TComponent>
        void MarkComponentDirty(EntityID eid)
        {
            auto e = ResolveEntity(eid);
            if (e == entt::null)
            {
                return;
            }
            GetDirtyIndex<TComponent>().Mark(EntityIndex(static_cast<uint32>(entt::to_integral(e))));
        }

        /**
         * @brief 获取组件脏索引（每组件类型一份，函数内 static）
         *
         * C++ 模板成员不能直接存容器——用函数模板内 static 局部变量，
         * 每 TComponent 特化独立一份。生命周期随程序，Scene 销毁时自动复用。
         */
        template <typename TComponent>
        DirtyIndex<TComponent> &GetDirtyIndex()
        {
            static DirtyIndex<TComponent> index;
            return index;
        }

        entt::registry &GetEnttRegistry()
        {
            return _entt;
        }

        const entt::registry &GetEnttRegistry() const
        {
            return _entt;
        }

        EntityRegistry &GetEntityRegistry()
        {
            return _registry;
        }

        const EntityRegistry &GetEntityRegistry() const
        {
            return _registry;
        }

        // ── 空间索引（per-scene）──

        /**
         * @brief 获取格子空间索引
         *
         * Grid 是 per-scene 的（场景内实体归属同一格子集）。
         * AOI 系统每帧更新它，空间查询（EntitiesInRadius 等）读它。
         */
        ECS::Grid &GetGrid()
        {
            return _grid;
        }

        const ECS::Grid &GetGrid() const
        {
            return _grid;
        }

        // ── 活跃集（AOI 内实体）──

        ActiveSet &GetActiveSet()
        {
            return _activeSet;
        }

        const ActiveSet &GetActiveSet() const
        {
            return _activeSet;
        }

    private:
        uint16         _sceneID {0};
        EntityRegistry _registry;
        entt::registry _entt;
        ECS::Grid      _grid;      // 格子空间索引（默认 10 世界单位/格）
        ActiveSet      _activeSet; // 活跃实体集
    };
} // namespace MMO::ECS
