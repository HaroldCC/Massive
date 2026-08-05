/**
 * @file WorldBridge.cpp
 * @brief World 专用 daScript Bridge 绑定——脚本读/写实体组件（ECS_06 §2）
 *
 * 铁律 1：脚本只拿 uint64 EntityID + 值拷贝（float3/int32 等 POD），
 *        不持组件引用。写回经 Bridge 内部 MarkComponentDirty。
 * 铁律 2：Bridge 调用量 ∝ 事件数/低频决策，绝不 ∝ 实体数 × tick。
 *
 * 场景访问：经 DasLangEngine::GetIns().GetScriptHost() → IDasLangHost
 *          （WorldServer 实现 GetDefaultScene/GetSceneByEntityID）
 */

#include "World/DasModule/WorldBridge.h"

#include "Common/ECS/EntityID.h"
#include "Common/ECS/Scene.h"
#include "Common/Log/Log.h"
#include "ScriptEngine/DasEngine.h"
#include "ScriptEngine/Module/DasCommonModule.h" // cast<ECS::EntityID> 特化（EntityID 值绑定）
#include "ScriptEngine/IDasHost.h"
#include "World/Component/Health.h"
#include "World/Component/Position.h"
#include "World/Component/Tags.h"

#include <daScript/ast/ast_interop.h>

namespace MMO
{

    namespace
    {
        /** @brief 经宿主拿场景（默认场景） */
        ECS::Scene *GetDefaultScene()
        {
            auto *host = DasLangEngine::GetIns().GetScriptHost();
            return host ? host->GetDefaultScene() : nullptr;
        }

        /** @brief 经宿主按 EntityID 定位场景（EntityID 隐式转 uint64 传给 host） */
        ECS::Scene *GetSceneFor(ECS::EntityID entityID)
        {
            auto *host = DasLangEngine::GetIns().GetScriptHost();
            return host ? host->GetSceneByEntityID(static_cast<uint64>(entityID)) : nullptr;
        }
    } // namespace

    // ── 实体生命周期 ──

    ECS::EntityID Bridge_EntityCreate()
    {
        auto *scene = GetDefaultScene();
        if (!scene)
        {
            return ECS::kInvalidEntityID;
        }
        return scene->CreateEntity();
    }

    void Bridge_EntityDestroy(ECS::EntityID entityID)
    {
        auto *scene = GetSceneFor(entityID);
        if (scene)
        {
            scene->DestroyEntity(entityID);
        }
    }

    bool Bridge_EntityIsValid(ECS::EntityID entityID)
    {
        auto *scene = GetSceneFor(entityID);
        return scene && scene->IsValidEntity(entityID);
    }

    // ── 位置 ──

    vec4f Bridge_EntityGetPosition(ECS::EntityID entityID, das::Context *, das::LineInfoArg *)
    {
        auto *scene = GetSceneFor(entityID);
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (scene)
        {
            auto e = scene->ResolveEntity(entityID);
            if (e != entt::null && scene->GetEnttRegistry().all_of<Position>(e))
            {
                auto &pos = scene->GetEnttRegistry().get<Position>(e);
                x         = pos.x;
                y         = pos.y;
                z         = pos.z;
            }
        }
        // vec4f（__m128）= 4 float，w 位未用（das float3 取前 3）
        return v_make_vec4f(x, y, z, 0.0f);
    }

    void Bridge_EntitySetPosition(ECS::EntityID entityID, vec4f pos, das::Context *, das::LineInfoArg *)
    {
        auto *scene = GetSceneFor(entityID);
        if (!scene)
        {
            return;
        }
        auto e = scene->ResolveEntity(entityID);
        if (e == entt::null)
        {
            return;
        }
        auto &dst = scene->GetEnttRegistry().get_or_emplace<Position>(e);
        // vec4f → float3：取前 3 分量
        dst.x = v_extract_x(pos);
        dst.y = v_extract_y(pos);
        dst.z = v_extract_z(pos);
        scene->MarkComponentDirty<Position>(entityID);
    }

    // ── 血量 ──

    int32 Bridge_EntityGetHp(ECS::EntityID entityID)
    {
        auto *scene = GetSceneFor(entityID);
        if (!scene)
        {
            return 0;
        }
        auto e = scene->ResolveEntity(entityID);
        if (e == entt::null || !scene->GetEnttRegistry().all_of<Health>(e))
        {
            return 0;
        }
        return scene->GetEnttRegistry().get<Health>(e).current;
    }

    void Bridge_EntitySetHp(ECS::EntityID entityID, int32 hp, das::Context *, das::LineInfoArg *)
    {
        auto *scene = GetSceneFor(entityID);
        if (!scene)
        {
            return;
        }
        auto e = scene->ResolveEntity(entityID);
        if (e == entt::null)
        {
            return;
        }
        scene->GetEnttRegistry().get_or_emplace<Health>(e).current = hp;
        scene->MarkComponentDirty<Health>(entityID);
    }

    int32 Bridge_EntityGetMaxHp(ECS::EntityID entityID)
    {
        auto *scene = GetSceneFor(entityID);
        if (!scene)
        {
            return 0;
        }
        auto e = scene->ResolveEntity(entityID);
        if (e == entt::null || !scene->GetEnttRegistry().all_of<Health>(e))
        {
            return 0;
        }
        return scene->GetEnttRegistry().get<Health>(e).max;
    }

    // ── 标签查询 ──

    bool Bridge_EntityIsDead(ECS::EntityID entityID)
    {
        auto *scene = GetSceneFor(entityID);
        if (!scene)
        {
            return false;
        }
        auto e = scene->ResolveEntity(entityID);
        return e != entt::null && scene->GetEnttRegistry().all_of<DeadTag>(e);
    }

    bool Bridge_EntityIsInCombat(ECS::EntityID entityID)
    {
        auto *scene = GetSceneFor(entityID);
        if (!scene)
        {
            return false;
        }
        auto e = scene->ResolveEntity(entityID);
        return e != entt::null && scene->GetEnttRegistry().all_of<CombatTag>(e);
    }

    bool Bridge_EntityIsStunned(ECS::EntityID entityID)
    {
        auto *scene = GetSceneFor(entityID);
        if (!scene)
        {
            return false;
        }
        auto e = scene->ResolveEntity(entityID);
        return e != entt::null && scene->GetEnttRegistry().all_of<StunnedTag>(e);
    }

    bool Bridge_EntityIsPlayer(ECS::EntityID entityID)
    {
        auto *scene = GetSceneFor(entityID);
        if (!scene)
        {
            return false;
        }
        auto e = scene->ResolveEntity(entityID);
        return e != entt::null && scene->GetEnttRegistry().all_of<PlayerTag>(e);
    }

    bool Bridge_EntityIsMonster(ECS::EntityID entityID)
    {
        auto *scene = GetSceneFor(entityID);
        if (!scene)
        {
            return false;
        }
        auto e = scene->ResolveEntity(entityID);
        return e != entt::null && scene->GetEnttRegistry().all_of<MonsterTag>(e);
    }

    // ── 注册 ──

    void RegisterWorldBridge(das::Module &mod, das::ModuleLibrary &lib)
    {
        using namespace das; // DAS_BIND_FUN 宏需要 das 命名空间

        // 实体生命周期
        das::addExtern<DAS_BIND_FUN(Bridge_EntityCreate)>(mod,
                                                          lib,
                                                          "EntityCreate",
                                                          das::SideEffects::modifyExternal,
                                                          "MMO::Bridge_EntityCreate");
        das::addExtern<DAS_BIND_FUN(Bridge_EntityDestroy)>(mod,
                                                           lib,
                                                           "EntityDestroy",
                                                           das::SideEffects::modifyExternal,
                                                           "MMO::Bridge_EntityDestroy");
        das::addExtern<DAS_BIND_FUN(Bridge_EntityIsValid)>(
            mod,
            lib,
            "EntityIsValid",
            das::SideEffects::accessExternal, // 读 scene 外部状态
            "MMO::Bridge_EntityIsValid");

        // 位置（das 签名用 float3，C++ 实现用 vec4f——addExternEx 自动转换）
        // entityID 参数用强类型 ECS::EntityID（脚本侧 EntityID 值类型）
        das::addExternEx<float3(ECS::EntityID, das::Context *, das::LineInfoArg *),
                         DAS_BIND_FUN(Bridge_EntityGetPosition)>(
            mod,
            lib,
            "EntityGetPosition",
            das::SideEffects::accessExternal, // 读 scene 外部状态
            "MMO::Bridge_EntityGetPosition")
            ->args({"entityID", "context", "at"});
        das::addExternEx<void(ECS::EntityID, float3, das::Context *, das::LineInfoArg *),
                         DAS_BIND_FUN(Bridge_EntitySetPosition)>(mod,
                                                                 lib,
                                                                 "EntitySetPosition",
                                                                 das::SideEffects::modifyExternal,
                                                                 "MMO::Bridge_EntitySetPosition")
            ->args({"entityID", "pos", "context", "at"});

        // 血量
        das::addExtern<DAS_BIND_FUN(Bridge_EntityGetHp)>(
            mod,
            lib,
            "EntityGetHp",
            das::SideEffects::accessExternal, // 读 scene 外部状态
            "MMO::Bridge_EntityGetHp");
        das::addExtern<DAS_BIND_FUN(Bridge_EntitySetHp)>(mod,
                                                         lib,
                                                         "EntitySetHp",
                                                         das::SideEffects::modifyExternal,
                                                         "MMO::Bridge_EntitySetHp")
            ->args({"entityID", "hp", "context", "at"});
        das::addExtern<DAS_BIND_FUN(Bridge_EntityGetMaxHp)>(
            mod,
            lib,
            "EntityGetMaxHp",
            das::SideEffects::accessExternal, // 读 scene 外部状态
            "MMO::Bridge_EntityGetMaxHp");

        // 标签
        das::addExtern<DAS_BIND_FUN(Bridge_EntityIsDead)>(
            mod,
            lib,
            "EntityIsDead",
            das::SideEffects::accessExternal, // 读 scene 外部状态
            "MMO::Bridge_EntityIsDead");
        das::addExtern<DAS_BIND_FUN(Bridge_EntityIsInCombat)>(
            mod,
            lib,
            "EntityIsInCombat",
            das::SideEffects::accessExternal, // 读 scene 外部状态
            "MMO::Bridge_EntityIsInCombat");
        das::addExtern<DAS_BIND_FUN(Bridge_EntityIsStunned)>(
            mod,
            lib,
            "EntityIsStunned",
            das::SideEffects::accessExternal, // 读 scene 外部状态
            "MMO::Bridge_EntityIsStunned");
        das::addExtern<DAS_BIND_FUN(Bridge_EntityIsPlayer)>(
            mod,
            lib,
            "EntityIsPlayer",
            das::SideEffects::accessExternal, // 读 scene 外部状态
            "MMO::Bridge_EntityIsPlayer");
        das::addExtern<DAS_BIND_FUN(Bridge_EntityIsMonster)>(
            mod,
            lib,
            "EntityIsMonster",
            das::SideEffects::accessExternal, // 读 scene 外部状态
            "MMO::Bridge_EntityIsMonster");
    }

} // namespace MMO