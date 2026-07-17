/**
 * @file MassiveModule.cpp
 * @brief MassiveModule 实现 — Phase 2 完整桥接层
 *
 * 15 个函数分 5 组：空间查询、属性查询、Tag 判断、世界交互、定时器。
 * 所有桥接函数通过全局 g_massiveMod 指针访问 C++ 世界。
 */

#include "Common/ECS/MassiveModule.h"

#include <chrono>

#include "Common/ECS/Scene.h"
#include "Common/Log/Log.h"
#include "World/Component/BattleStats.h"
#include "World/Component/Health.h"
#include "World/Component/Position.h"
#include "World/Component/Tags.h"
#include "World/Component/Velocity.h"
#include "World/SceneManager.h"
#include "World/WorldServer.h"
#include "World/WorldSession.h"

#include <daScript/simulate/aot.h>
#include <daScript/simulate/simulate.h>
#include <daScript/ast/ast_handle.h>

// ── 全局指针：BindFunctions 时设定，所有静态桥接函数通过它访问上下文 ──
namespace
{
    MMO::MassiveModule *g_massiveMod = nullptr;
} // namespace

using namespace das;
using namespace MMO;

// ═══════════════════════════════════════════════════════════════
// 辅助函数
// ═══════════════════════════════════════════════════════════════

namespace
{

    struct ResolvedEntity
    {
        ECS::Scene  *scene = nullptr;
        bool         valid = false;
        entt::entity e     = entt::null;
    };

    ResolvedEntity ResolveEntity(uint64_t fullEntityId)
    {
        uint32_t entityID = static_cast<uint32_t>(fullEntityId & 0xFFFFFFFF);
        uint32_t sceneID  = static_cast<uint32_t>(fullEntityId >> 32);

        auto *sceneMgr = g_massiveMod ? g_massiveMod->_sceneMgr : nullptr;
        auto *scene    = sceneMgr ? sceneMgr->GetScene(sceneID) : nullptr;

        if (!scene || !scene->IsValid(Entity{sceneID, entityID}))
        {
            return {nullptr, false, entt::null};
        }
        return {scene, true, entt::entity(entityID)};
    }

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════
// 3.1 空间查询
// ═══════════════════════════════════════════════════════════════

static das::float3 Bridge_EntityPosition(uint64_t fullEntityId)
{
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    if (!valid)
    {
        return das::float3();
    }

    Entity ent{scene->SceneID(), static_cast<uint32_t>(e)};
    if (!scene->HasComponent<Position>(ent))
    {
        return das::float3();
    }

    auto &pos = scene->GetComponent<Position>(ent);
    return das::float3(pos.x, pos.y, pos.z);
}

static das::TArray<uint64_t> Bridge_EntitiesInRadius(const das::float3 &center,
                                                     float               radius)
{
    das::TArray<uint64_t> result;

    auto *sceneMgr = g_massiveMod ? g_massiveMod->_sceneMgr : nullptr;
    auto *scene    = sceneMgr ? sceneMgr->GetDefaultScene() : nullptr;
    if (!scene)
    {
        return result;
    }

    auto view = scene->Registry().view<const Position>();
    for (auto [e, pos] : view.each())
    {
        float dx = pos.x - center.x;
        float dz = pos.z - center.z;
        if (dx * dx + dz * dz <= radius * radius)
        {
            uint32_t eid    = static_cast<uint32_t>(entt::to_integral(e));
            uint64_t fullID = (static_cast<uint64_t>(scene->SceneID()) << 32) | eid;
            reinterpret_cast<uint64_t *>(result.data)[result.size] = fullID;
            result.size++;
        }
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// 3.2 属性查询
// ═══════════════════════════════════════════════════════════════

static BattleStats *Bridge_EntityGetBattleStats(uint64_t fullEntityId)
{
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    if (!valid)
    {
        return nullptr;
    }

    Entity ent{scene->SceneID(), static_cast<uint32_t>(e)};

    if (!scene->HasComponent<BattleStats>(ent) && !scene->HasComponent<Health>(ent))
    {
        return nullptr;
    }

    auto *ctx   = g_massiveMod ? g_massiveMod->GetContext() : nullptr;
    auto *stats = ctx ? reinterpret_cast<BattleStats *>(
                            ctx->allocate(sizeof(BattleStats)))
                      : nullptr;
    if (!stats)
    {
        return nullptr;
    }
    *stats = {};

    if (scene->HasComponent<BattleStats>(ent))
    {
        *stats = scene->GetComponent<BattleStats>(ent);
    }

    if (scene->HasComponent<Health>(ent))
    {
        auto &hp       = scene->GetComponent<Health>(ent);
        stats->currentHp = hp.current;
        stats->maxHp     = hp.max;
    }

    return stats;
}

// ═══════════════════════════════════════════════════════════════
// 3.3 Tag 判断
// ═══════════════════════════════════════════════════════════════

static bool Bridge_EntityIsDead(uint64_t fullEntityId)
{
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<DeadTag>(e);
}

static bool Bridge_EntityIsInCombat(uint64_t fullEntityId)
{
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<CombatTag>(e);
}

static bool Bridge_EntityIsStunned(uint64_t fullEntityId)
{
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<StunnedTag>(e);
}

static bool Bridge_EntityIsPlayer(uint64_t fullEntityId)
{
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<PlayerTag>(e);
}

static bool Bridge_EntityIsMonster(uint64_t fullEntityId)
{
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<MonsterTag>(e);
}

// ═══════════════════════════════════════════════════════════════
// 3.4 世界交互
// ═══════════════════════════════════════════════════════════════

static uint64_t Bridge_CreateEntity(das::float3 pos, int entityType)
{
    auto *sceneMgr = g_massiveMod ? g_massiveMod->_sceneMgr : nullptr;
    auto *scene    = sceneMgr ? sceneMgr->GetDefaultScene() : nullptr;
    if (!scene)
    {
        Log::Warn("MassiveModule::CreateEntity: no default scene");
        return 0;
    }

    auto     e        = scene->Registry().create();
    uint32_t entityID = static_cast<uint32_t>(entt::to_integral(e));
    Entity   entity{scene->SceneID(), entityID};

    scene->EmplaceComponent<Position>(entity, pos.x, pos.y, pos.z);

    if (entityType == 1)
    {
        scene->Registry().emplace<PlayerTag>(e);
    }
    else
    {
        scene->Registry().emplace<MonsterTag>(e);
    }

    scene->EmplaceComponent<Health>(entity, 100, 100);

    Log::Debug("MassiveModule::CreateEntity: eid={} type={} pos=({:.1f},{:.1f},{:.1f})",
               entityID, entityType, pos.x, pos.y, pos.z);

    return (static_cast<uint64_t>(scene->SceneID()) << 32) | entityID;
}

static void Bridge_DestroyEntity(uint64_t fullEntityId)
{
    uint32_t entityID = static_cast<uint32_t>(fullEntityId & 0xFFFFFFFF);
    uint32_t sceneID  = static_cast<uint32_t>(fullEntityId >> 32);

    auto *sceneMgr = g_massiveMod ? g_massiveMod->_sceneMgr : nullptr;
    auto *scene    = sceneMgr ? sceneMgr->GetScene(sceneID) : nullptr;
    if (!scene)
    {
        return;
    }

    scene->DestroyEntity(Entity{sceneID, entityID});
    Log::Debug("MassiveModule::DestroyEntity: eid={}", entityID);
}

static void Bridge_SendToClient(uint32_t sessionID, uint32_t msgID,
                                const das::TArray<uint8_t> &data)
{
    auto *worldServer = g_massiveMod ? g_massiveMod->_worldServer : nullptr;
    if (!worldServer)
    {
        Log::Warn("MassiveModule::SendToClient: no WorldServer");
        return;
    }

    worldServer->SendRawToClient(sessionID, msgID,
                                 reinterpret_cast<const uint8 *>(data.data),
                                 static_cast<size_t>(data.size));
}

// ═══════════════════════════════════════════════════════════════
// 3.5 定时器
// ═══════════════════════════════════════════════════════════════

static uint32_t Bridge_ScheduleTimer(int delayMs,
                                     const das::TBlock<void, uint32_t> &block)
{
    if (!g_massiveMod || !g_massiveMod->_timingWheel)
    {
        Log::Warn("MassiveModule::ScheduleTimer: no TimingWheel");
        return 0;
    }

    auto    &mod     = *g_massiveMod;
    uint32_t timerID = mod._nextTimerID.fetch_add(1, std::memory_order_relaxed);

    auto ctx = mod._ctx;
    mod._timerCallbacks[timerID] = {block, ctx};

    mod._timingWheel->Schedule(std::chrono::milliseconds(delayMs),
                               [timerID, ctx, &mod]() {
                                   auto it = mod._timerCallbacks.find(timerID);
                                   if (it != mod._timerCallbacks.end())
                                   {
                                       das_invoke<void>::invoke(it->second.ctx.get(), nullptr,
                                                                it->second.block, timerID);
                                       mod._timerCallbacks.erase(it);
                                   }
                               });

    Log::Debug("MassiveModule::ScheduleTimer: id={} delayMs={}", timerID, delayMs);
    return timerID;
}

static void Bridge_CancelTimer(uint32_t timerID)
{
    if (!g_massiveMod)
    {
        return;
    }

    g_massiveMod->_timerCallbacks.erase(timerID);

    if (g_massiveMod->_timingWheel)
    {
        g_massiveMod->_timingWheel->Cancel(timerID);
    }

    Log::Debug("MassiveModule::CancelTimer: id={}", timerID);
}

// ═══════════════════════════════════════════════════════════════
// 3.6 工具函数
// ═══════════════════════════════════════════════════════════════

static float Bridge_GetDeltaTime()
{
    return 0.02f;
}

static uint64_t Bridge_FindEntityBySession(uint32_t sessionID)
{
    auto *sessions = g_massiveMod ? g_massiveMod->_sessions : nullptr;
    if (!sessions)
    {
        return 0;
    }

    auto it = sessions->find(sessionID);
    if (it == sessions->end())
    {
        return 0;
    }

    auto &entity = it->second.entity;
    return (static_cast<uint64_t>(entity.sceneId) << 32) | entity.entityId;
}

// ── 日志函数 ──

static void Bridge_LogInfo(const char *text, das::Context * /*context*/, das::LineInfoArg *at)
{
    if (at && at->fileInfo)
    {
        Log::At(ELogLevel::Info, at->fileInfo->name.c_str(),
                static_cast<int>(at->line), "{}", text ? text : "(null)");
    }
    else
    {
        Log::Info("{}", text ? text : "(null)");
    }
}

static void Bridge_LogWarn(const char *text, das::Context * /*context*/, das::LineInfoArg *at)
{
    if (at && at->fileInfo)
    {
        Log::At(ELogLevel::Warn, at->fileInfo->name.c_str(),
                static_cast<int>(at->line), "{}", text ? text : "(null)");
    }
    else
    {
        Log::Warn("{}", text ? text : "(null)");
    }
}

static void Bridge_LogError(const char *text, das::Context * /*context*/, das::LineInfoArg *at)
{
    if (at && at->fileInfo)
    {
        Log::At(ELogLevel::Error, at->fileInfo->name.c_str(),
                static_cast<int>(at->line), "{}", text ? text : "(null)");
    }
    else
    {
        Log::Error("{}", text ? text : "(null)");
    }
}

// ═══════════════════════════════════════════════════════════════
// MassiveModule 构造 / 析构 / 注册
// ═══════════════════════════════════════════════════════════════

namespace MMO
{

    MassiveModule::MassiveModule(WorldServer                              *worldServer,
                                 SceneManager                             *sceneMgr,
                                 TimingWheel                              *timingWheel,
                                 std::unordered_map<uint32, WorldSession> *sessions)
        : Module("massive")
        , _worldServer(worldServer)
        , _sceneMgr(sceneMgr)
        , _timingWheel(timingWheel)
        , _sessions(sessions)
    {
    }

    MassiveModule::~MassiveModule()
    {
        if (g_massiveMod == this)
        {
            g_massiveMod = nullptr;
        }
    }

    void MassiveModule::BindFunctions()
    {
        g_massiveMod = this;

        ModuleLibrary lib(this);
        lib.addBuiltInModule();
        auto *builtin = Module::require("$");
        if (builtin)
        {
            addBuiltinDependency(lib, builtin, true);
        }

        // BattleStats 和 EntitiesInRadius 依赖额外类型注册 → Phase 3
        // addAnnotation(MANAGED_TYPE_FACTORY...) + MAKE_EXTERNAL_TYPE_FACTORY

        // ── 3.1 空间查询 ──
        addExtern<DAS_BIND_FUN(Bridge_EntityPosition)>(*this, lib, "EntityPosition",
                                                        SideEffects::none);

        // ── 3.2 属性查询（需 BattleStats 类型注册）──
        // addExtern<DAS_BIND_FUN(Bridge_EntityGetBattleStats)>(*this, lib, "EntityGetBattleStats",
        //                                                       SideEffects::none);

        // ── 3.3 Tag 判断 ──
        addExtern<DAS_BIND_FUN(Bridge_EntityIsDead)>(*this, lib, "EntityIsDead",
                                                      SideEffects::none);
        addExtern<DAS_BIND_FUN(Bridge_EntityIsInCombat)>(*this, lib, "EntityIsInCombat",
                                                          SideEffects::none);
        addExtern<DAS_BIND_FUN(Bridge_EntityIsStunned)>(*this, lib, "EntityIsStunned",
                                                         SideEffects::none);
        addExtern<DAS_BIND_FUN(Bridge_EntityIsPlayer)>(*this, lib, "EntityIsPlayer",
                                                        SideEffects::none);
        addExtern<DAS_BIND_FUN(Bridge_EntityIsMonster)>(*this, lib, "EntityIsMonster",
                                                         SideEffects::none);

        // ── 3.4 世界交互 ──
        addExtern<DAS_BIND_FUN(Bridge_CreateEntity)>(*this, lib, "CreateEntity",
                                                      SideEffects::modifyExternal);
        addExtern<DAS_BIND_FUN(Bridge_DestroyEntity)>(*this, lib, "DestroyEntity",
                                                       SideEffects::modifyExternal);
        addExtern<DAS_BIND_FUN(Bridge_SendToClient)>(*this, lib, "SendToClient",
                                                      SideEffects::modifyExternal);

        // ── 3.5 定时器 ──
        addExtern<DAS_BIND_FUN(Bridge_ScheduleTimer)>(*this, lib, "ScheduleTimer",
                                                       SideEffects::modifyExternal);
        addExtern<DAS_BIND_FUN(Bridge_CancelTimer)>(*this, lib, "CancelTimer",
                                                     SideEffects::modifyExternal);

        // ── 3.6 工具函数 ──
        addExtern<DAS_BIND_FUN(Bridge_GetDeltaTime)>(*this, lib, "GetDeltaTime",
                                                      SideEffects::none);
        addExtern<DAS_BIND_FUN(Bridge_FindEntityBySession)>(*this, lib, "FindEntityBySession",
                                                             SideEffects::none);

        // ── 日志 ──
        addExtern<DAS_BIND_FUN(Bridge_LogInfo)>(*this, lib, "LogInfo",
                                                SideEffects::modifyExternal)
            ->args({"text", "context", "at"});
        addExtern<DAS_BIND_FUN(Bridge_LogWarn)>(*this, lib, "LogWarn",
                                                SideEffects::modifyExternal)
            ->args({"text", "context", "at"});
        addExtern<DAS_BIND_FUN(Bridge_LogError)>(*this, lib, "LogError",
                                                 SideEffects::modifyExternal)
            ->args({"text", "context", "at"});
    }

} // namespace MMO
