/**
 * @file DasBindings.cpp
 * @brief daScript 绑定：注册类型到脚本层并提供 BattleStats 访问器
 *
 * 本文件负责把 C++ 侧的简单 POD 类型（如 MMO::BattleStats）映射到 das，
 * 并提供字段访问器函数以便脚本可以读取按值返回的结构字段。
 *
 * 说明：当前为只读访问器（脚本无法直接写回 C++）。如需写回，需实现 managed
 * external type factory（另开 PR）。
 */

#include "Common/ECS/MassiveModule.h"
#include "World/Component/BattleStats.h"

#include <daScript/simulate/simulate.h>
#include <daScript/ast/ast_interop.h>

using namespace das;
using namespace MMO;

namespace MMO
{

    // ----------------------------------
    // BattleStats 字段访问器（只读）
    // ----------------------------------
    static int32 BattleStats_GetAttack(const BattleStats &s)
    {
        return s.attack;
    }

    static int32 BattleStats_GetDefense(const BattleStats &s)
    {
        return s.defense;
    }

    static int32 BattleStats_GetMagicAttack(const BattleStats &s)
    {
        return s.magicAttack;
    }

    static int32 BattleStats_GetMagicDefense(const BattleStats &s)
    {
        return s.magicDefense;
    }

    static int32 BattleStats_GetCritRate(const BattleStats &s)
    {
        return s.critRate;
    }

    static int32 BattleStats_GetCritDamage(const BattleStats &s)
    {
        return s.critDamage;
    }

    static int32 BattleStats_GetDodgeRate(const BattleStats &s)
    {
        return s.dodgeRate;
    }

    static int32 BattleStats_GetHitRate(const BattleStats &s)
    {
        return s.hitRate;
    }

    static int32 BattleStats_GetAttackSpeed(const BattleStats &s)
    {
        return s.attackSpeed;
    }

    static int32 BattleStats_GetMoveSpeed(const BattleStats &s)
    {
        return s.moveSpeed;
    }

    static int32 BattleStats_GetCurrentHp(const BattleStats &s)
    {
        return s.currentHp;
    }

    static int32 BattleStats_GetMaxHp(const BattleStats &s)
    {
        return s.maxHp;
    }

    static int32 BattleStats_GetCurrentMp(const BattleStats &s)
    {
        return s.currentMp;
    }

    static int32 BattleStats_GetMaxMp(const BattleStats &s)
    {
        return s.maxMp;
    }

    /**
     * @brief 注册 BattleStats 类型与访问器到 daScript 模块
     *
     * @param mod  模块实例（Das Module）
     * @param lib  ModuleLibrary（类型创建依赖）
     */
    void RegisterDasBindings(Module &mod, ModuleLibrary &lib)
    {
        // 确保 daScript 类型系统中存在 BattleStats 的类型声明
        (void)makeType<BattleStats>(lib);

        // 注册字段访问器为全局函数，脚本可以通过这些函数读取返回的 BattleStats
        // 示例脚本： let s = EntityGetBattleStats(id); let a = BattleStats_Attack(s);
        addExtern<DAS_BIND_FUN(BattleStats_GetAttack)>(mod, lib, "BattleStats_Attack", SideEffects::none);
        addExtern<DAS_BIND_FUN(BattleStats_GetDefense)>(mod, lib, "BattleStats_Defense", SideEffects::none);
        addExtern<DAS_BIND_FUN(BattleStats_GetMagicAttack)>(mod,
                                                            lib,
                                                            "BattleStats_MagicAttack",
                                                            SideEffects::none);
        addExtern<DAS_BIND_FUN(BattleStats_GetMagicDefense)>(mod,
                                                             lib,
                                                             "BattleStats_MagicDefense",
                                                             SideEffects::none);
        addExtern<DAS_BIND_FUN(BattleStats_GetCritRate)>(mod, lib, "BattleStats_CritRate", SideEffects::none);
        addExtern<DAS_BIND_FUN(BattleStats_GetCritDamage)>(mod,
                                                           lib,
                                                           "BattleStats_CritDamage",
                                                           SideEffects::none);
        addExtern<DAS_BIND_FUN(BattleStats_GetDodgeRate)>(mod,
                                                          lib,
                                                          "BattleStats_DodgeRate",
                                                          SideEffects::none);
        addExtern<DAS_BIND_FUN(BattleStats_GetHitRate)>(mod, lib, "BattleStats_HitRate", SideEffects::none);
        addExtern<DAS_BIND_FUN(BattleStats_GetAttackSpeed)>(mod,
                                                            lib,
                                                            "BattleStats_AttackSpeed",
                                                            SideEffects::none);
        addExtern<DAS_BIND_FUN(BattleStats_GetMoveSpeed)>(mod,
                                                          lib,
                                                          "BattleStats_MoveSpeed",
                                                          SideEffects::none);
        addExtern<DAS_BIND_FUN(BattleStats_GetCurrentHp)>(mod,
                                                          lib,
                                                          "BattleStats_CurrentHp",
                                                          SideEffects::none);
        addExtern<DAS_BIND_FUN(BattleStats_GetMaxHp)>(mod, lib, "BattleStats_MaxHp", SideEffects::none);
        addExtern<DAS_BIND_FUN(BattleStats_GetCurrentMp)>(mod,
                                                          lib,
                                                          "BattleStats_CurrentMp",
                                                          SideEffects::none);
        addExtern<DAS_BIND_FUN(BattleStats_GetMaxMp)>(mod, lib, "BattleStats_MaxMp", SideEffects::none);
    }

} // namespace MMO
