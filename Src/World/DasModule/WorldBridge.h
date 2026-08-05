/**
 * @file WorldBridge.h
 * @brief World 专用 daScript Bridge 绑定声明（ECS_06 §2）
 *
 * 脚本侧 `require world` 后可调用 EntityCreate/EntityGetPosition 等。
 * 注册函数 RegisterWorldBridge 由 WorldScriptModule::Build 调用。
 */
#pragma once

#include "Common/Core/Types.h"
#include "Common/ECS/EntityID.h"

#include "daScript/ast/ast.h"
#include "daScript/daScriptModule.h"
#include "vecmath/dag_vecMathDecl.h"

namespace MMO
{

    // ── Bridge_* 导出函数（实现见 WorldBridge.cpp）──
    // 注意：float3 跨语言用 vec4f（__m128），绑定用 addExternEx 显式签名
    // entityID 参数/返回用强类型 ECS::EntityID（脚本侧 ManagedValueAnnotation 值类型，
    // 与 sessionID/accountID 强区分）——值经 cast<EntityID> 特化走 vec4f 寄存器。

    ECS::EntityID Bridge_EntityCreate();
    void          Bridge_EntityDestroy(ECS::EntityID entityID);
    bool          Bridge_EntityIsValid(ECS::EntityID entityID);

    vec4f Bridge_EntityGetPosition(ECS::EntityID entityID, das::Context *ctx, das::LineInfoArg *at);
    void Bridge_EntitySetPosition(ECS::EntityID entityID, vec4f pos, das::Context *ctx, das::LineInfoArg *at);

    int32 Bridge_EntityGetHp(ECS::EntityID entityID);
    void  Bridge_EntitySetHp(ECS::EntityID entityID, int32 hp, das::Context *ctx, das::LineInfoArg *at);
    int32 Bridge_EntityGetMaxHp(ECS::EntityID entityID);

    bool Bridge_EntityIsDead(ECS::EntityID entityID);
    bool Bridge_EntityIsInCombat(ECS::EntityID entityID);
    bool Bridge_EntityIsStunned(ECS::EntityID entityID);
    bool Bridge_EntityIsPlayer(ECS::EntityID entityID);
    bool Bridge_EntityIsMonster(ECS::EntityID entityID);

    /**
     * @brief 注册全部 World Bridge 绑定到模块
     * @param mod 目标模块（world）
     * @param lib 模块库
     */
    void RegisterWorldBridge(das::Module &mod, das::ModuleLibrary &lib);

} // namespace MMO