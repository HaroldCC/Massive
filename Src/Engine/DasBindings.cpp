/**
 * @file DasBindings.cpp
 * @brief daScript 绑定：注册类型到脚本层
 *
 * 当前文件负责把 C++ 侧的简单 POD 类型（如 MMO::BattleStats）映射到 das
 *，使得按值返回的结构能在脚本端被正确解析。
 */

#include "Common/ECS/MassiveModule.h"
#include "World/Component/BattleStats.h"

#include <daScript/simulate/simulate.h>
#include <daScript/ast/ast_interop.h>

using namespace das;
using namespace MMO;

namespace MMO
{

/**
 * @brief 注册 BattleStats 到 daScript，使得 C++ 按值返回的 BattleStats 能被脚本侧读取
 *
 * 说明：目前仅在类型系统中注册 BattleStats（确保 addExtern 在绑定时能够识别返回类型）。
 *       该注册并不暴露字段访问器；若需脚本通过 `.attack` 访问字段，可在后续 PR 中
 *       使用 addExternProperty 或 addExternPropertyForType 显式注册字段访问器。
 */
static void RegisterBattleStats(Module & /*mod*/, ModuleLibrary &lib)
{
    // 确保 daScript 类型系统中存在 BattleStats 的类型声明。
    // makeType<T>(lib) 会在 ModuleLibrary 中生成或查找对应的 TypeDecl，
    // 使得对该类型的 addExtern 可以顺利进行。
    (void) makeType<BattleStats>(lib);
}

void RegisterDasBindings(Module &mod, ModuleLibrary &lib)
{
    // 目前只注册 BattleStats 类型（按值返回的最小支持）
    RegisterBattleStats(mod, lib);
}

} // namespace MMO
