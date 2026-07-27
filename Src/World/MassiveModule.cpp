/**
 * @file MassiveModule.cpp
 * @brief MassiveModule 实现 — Phase 2 完整桥接层（安全修复）
 *
 * 本次改动目的：修复脚本桥接层中的不安全内存操作与裸指针返回。
 * - 使用 DasHelpers::CreateDasArrayFromVector 在 das::Context 中安全分配并构造 das::TArray
 * - 将 EntityGetBattleStats 改为按值返回 MMO::BattleStats（避免 ctx->allocate 裸指针）
 * - 定时器回调继续使用共享的 das::ContextPtr 保证生命周期（类型定义已在头文件中）
 *
 * 说明：本文件保留原有分组与注册逻辑，仅替换不安全实现为受控实现，注册部分暂不变。
 */

#include "Common/ECS/MassiveModule.h"

#include <chrono>

#include <daScript/simulate/aot.h>
#include <daScript/simulate/simulate.h>
#include <daScript/ast/ast_handle.h>

#include "Common/ECS/Scene.h"
#include "Common/Log/Log.h"
#include "World/Component/BattleStats.h"
#include "World/Component/EntityType.h"
#include "World/Component/Health.h"
#include "World/Component/Position.h"
#include "World/Component/Tags.h"
#include "World/Component/Velocity.h"
#include "World/SceneManager.h"
#include "World/WorldServer.h"
#include "World/WorldSession.h"

#include "Engine/DasHelpers.h"

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

    ResolvedEntity ResolveEntity(uint64 fullEntityId)
    {
        uint32 entityID = static_cast<uint32>(fullEntityId & 0xFFFFFFFF);
        uint32 sceneID  = static_cast<uint32>(fullEntityId >> 32);

        auto *sceneMgr = g_massiveMod ? g_massiveMod->_sceneMgr : nullptr;
        auto *scene    = sceneMgr ? sceneMgr->GetScene(sceneID) : nullptr;

        if (!scene || !scene->IsValid(Entity{sceneID, entityID}))
        {
            return {nullptr, false, entt::null};
        }
        return {scene, true, entt::entity(entityID)};
    }

} // anonymous namespace

/** @name 空间查询 */
/** @{ */

static das::float3 Bridge_EntityPosition(uint64 fullEntityId)
{
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    if (!valid)
    {
        return das::float3();
    }

    Entity ent{scene->SceneID(), static_cast<uint32>(e)};
    if (!scene->HasComponent<Position>(ent))
    {
        return das::float3();
    }

    auto &pos = scene->GetComponent<Position>(ent);
    return das::float3(pos.x, pos.y, pos.z);
}

/**
 * @brief 在给定半径内查询实体
 *
 * 注意：该实现改为先在 C++ 侧收集结果到 std::vector，再通过 DasHelpers
 * 在 das::Context 中分配并构造 das::TArray 返回，避免直接写入 TArray 内部内存。
 *
 * @param center 查询中心
 * @param radius 半径
 * @param ctx    调用时传入的 das::Context（由绑定注册时传入）
 * @return das::TArray<uint64> 返回的实体 fullID 列表（sceneID<<32 | entityID）
 */
static das::TArray<uint64> Bridge_EntitiesInRadius(const das::float3 &center,
                                                   float               radius,
                                                   das::Context       *ctx)
{
    std::vector<uint64_t> tmp;

    auto *sceneMgr = g_massiveMod ? g_massiveMod->_sceneMgr : nullptr;
    auto *scene    = sceneMgr ? sceneMgr->GetDefaultScene() : nullptr;
    if (!scene || !ctx)
    {
        return das::TArray<uint64>();
    }

    auto view = scene->Registry().view<const Position>();
    for (auto [e, pos] : view.each())
    {
        float dx = pos.x - center.x;
        float dz = pos.z - center.z;
        if (dx * dx + dz * dz <= radius * radius)
        {
            uint32 eid    = static_cast<uint32>(entt::to_integral(e));
            uint64 fullID = (static_cast<uint64>(scene->SceneID()) << 32) | eid;
            tmp.push_back(fullID);
        }
    }

    return DasHelpers::CreateDasArrayFromVector<uint64>(ctx, tmp);
}

/** @} */

/** @name 属性查询 */
/** @{ */

/**
 * @brief 获取实体战斗属性
 *
 * 为避免在宿主堆上分配裸指针并将其返回给脚本（所有权不明确），函数改为按值返回
 * MMO::BattleStats（POD 结构体，拷贝开销极小）。
 *
 * @param fullEntityId 场景与实体组合 ID
 * @return MMO::BattleStats 按值返回的战斗属性快照
 */
static MMO::BattleStats Bridge_EntityGetBattleStats(uint64 fullEntityId)
{
    MMO::BattleStats stats{}; // default 初始化
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    if (!valid)
    {
        return stats;
    }

    Entity ent{scene->SceneID(), static_cast<uint32>(e)};

    if (scene->HasComponent<BattleStats>(ent))
    {
        stats = scene->GetComponent<BattleStats>(ent);
    }

    if (scene->HasComponent<Health>(ent))
    {
        auto &hp       = scene->GetComponent<Health>(ent);
        stats.currentHp = hp.current;
        stats.maxHp     = hp.max;
    }

    return stats;
}

/** @} */

/** @name Tag 判断 */
/** @{ */

static bool Bridge_EntityIsDead(uint64 fullEntityId)
{
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<DeadTag>(e);
}

static bool Bridge_EntityIsInCombat(uint64 fullEntityId)
{
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<CombatTag>(e);
}

static bool Bridge_EntityIsStunned(uint64 fullEntityId)
{
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<StunnedTag>(e);
}

static bool Bridge_EntityIsPlayer(uint64 fullEntityId)
{
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<PlayerTag>(e);
}

static bool Bridge_EntityIsMonster(uint64 fullEntityId)
{
    auto [scene, valid, e] = ResolveEntity(fullEntityId);
    return valid && scene->Registry().all_of<MonsterTag>(e);
}

/** @} */

/** @name 世界交互 */
/** @{ */

static uint64 Bridge_CreateEntity(das::float3 pos, int32 entityType)
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

    if (entityType == static_cast<int>(EEntityType::ENTITY_PLAYER))
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

static void Bridge_DestroyEntity(uint64 fullEntityId)
{
    uint32 entityID = static_cast<uint32>(fullEntityId & 0xFFFFFFFF);
    uint32 sceneID  = static_cast<uint32>(fullEntityId >> 32);

    auto *sceneMgr = g_massiveMod ? g_massiveMod->_sceneMgr : nullptr;
    auto *scene    = sceneMgr ? sceneMgr->GetScene(sceneID) : nullptr;
    if (!scene)
    {
        return;
    }

    scene->DestroyEntity(Entity{sceneID, entityID});
    Log::Debug("MassiveModule::DestroyEntity: eid={}", entityID);
}

/** @} */

/** @name 定时器 */
/** @{ */

static void Bridge_SendToClient(uint32 sessionID, uint32 msgID,
                                const das::TArray<uint8> &data)
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

/**
 * @brief ScheduleTimer：在 Module 的 TimingWheel 上注册回调。
 *
 * 注意：_timerCallbacks 中保存的是 das::Context 的 shared_ptr（见头文件定义），
 *       回调触发时直接在 TimingWheel 的执行上下文（LogicThread）中调用 das_invoke，
 *       因此只需保证 ctx 不被提前销毁。
 */
static uint32 Bridge_ScheduleTimer(int32 delayMs,
                                   const das::TBlock<void, uint32> &block)
{
    if (!g_massiveMod || !g_massiveMod->_timingWheel)
    {
        Log::Warn("MassiveModule::ScheduleTimer: no TimingWheel");
        return 0;
    }

    auto    &mod     = *g_massiveMod;
    uint32 timerID = mod._nextTimerID.fetch_add(1, std::memory_order_relaxed);

    // 使用 shared_ptr 保证 Context 在回调前不会被释放
    auto ctx = mod._ctx; // das::ContextPtr
    mod._timerCallbacks[timerID] = {block, ctx};

    // TimingWheel 的回调在 LogicThread 的上下文中执行（见 LogicThread 实现），
    // 因此可以安全调用 das_invoke。触发后清理回调条目。
    mod._timingWheel->Schedule(std::chrono::milliseconds(delayMs),
                               [timerID, &mod]() {
                                   auto it = mod._timerCallbacks.find(timerID);
                                   if (it != mod._timerCallbacks.end())
                                   {
                                       // it->second.ctx 是 shared_ptr<das::Context>
                                       das_invoke<void>::invoke(it->second.ctx.get(), nullptr,
                                                                it->second.block, timerID);
                                       mod._timerCallbacks.erase(it);
                                   }
                               });

    Log::Debug("MassiveModule::ScheduleTimer: id={} delayMs={}", timerID, delayMs);
    return timerID;
}

static void Bridge_CancelTimer(uint32 timerID)
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

/** @} */

/** @name 工具函数 */
/** @{ */

static float Bridge_GetDeltaTime()
{
    return 0.02f;
}

static uint64 Bridge_FindEntityBySession(uint32 sessionID)
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
    return (static_cast<uint64>(entity.sceneId) << 32) | entity.entityId;
}

/** @} */

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
