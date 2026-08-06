#include "WorldScriptModule.h"

#include "World/AutoGen/EventTypeRegistry.gen.h"
#include "World/AutoGen/GameEventBindings.gen.h"
#include "World/AutoGen/ProtoBindIndex.gen.h"
#include "World/DasModule/WorldBridge.h"
// addExtern/DAS_BIND_FUN 模板定义在 ast_interop.h（DasCommonModule 经 daScript.h 引入）
#include "daScript/daScript.h"

namespace MMO
{

    WorldScriptModule::WorldScriptModule() : das::Module("world")
    {
        das::ModuleLibrary lib(this);
        lib.addBuiltInModule();
        // 注册生成器产出的全部消息类型 + EMsgID 枚举（Common/World 消息）
        RegisterAllProtoMessageTypes(*this, lib);
        // 注册游戏事件类型 + EGameEventType 枚举（ECS_06 决策 3）
        RegisterAllGameEventBindings(*this, lib);
        // 注册 Bridge 实体操作绑定（脚本读/写组件）
        RegisterWorldBridge(*this, lib);
        // 事件宏命名查表（ScriptLayer_06 §3.1：命名约定单一真相源）
        // 事件类型（EGameEventType）绑在 world 模块，[game_event] 宏 require world 可见。
        // 纯查询（SideEffects::none）；cppName 全限定（AOT 发射需要）。
        das::addExtern<DAS_BIND_FUN(MMO::Script::EventTypeToID)>(
            *this, lib, "EventTypeToID", das::SideEffects::none, "MMO::Script::EventTypeToID")
            ->args({"typeName"});
    }

    das::ModuleAotType WorldScriptModule::aotRequire(das::TextWriter &tw) const
    {
        // AOT 生成端需要看到 World 侧绑定头（注册函数声明）：
        // 生成的 .das.cpp 会引用 RegisterAllProtoMessageTypes 注册的类型与枚举。
        tw << "#include \"World/AutoGen/ProtoBindIndex.gen.h\"\n";
        // 事件类型（GameEventEnvelope / *Event / EGameEventType）
        tw << "#include \"World/AutoGen/GameEventBindings.gen.h\"\n";
        // 事件宏命名查表（宏 apply 调用 EventTypeToID）
        tw << "#include \"World/AutoGen/EventTypeRegistry.gen.h\"\n";
        tw << "#include \"ScriptEngine/GameEventBus.h\"\n";
        // Bridge 实体操作（脚本调 EntityGetPosition 等）
        tw << "#include \"World/DasModule/WorldBridge.h\"\n";
        return das::ModuleAotType::cpp;
    }

} // namespace MMO

// 官方范式：REGISTER_MODULE 自注册 + 宿主 PULL_MODULE 拉取。
// 模块实例归 daScriptEnvironment 全局链表管理，das::Module::Shutdown() 统一释放，
// 不再由 WorldDasModule unique_ptr 持有（消除热重载重复 new 的 DAS_FATAL_ERROR 与 double-delete）。
REGISTER_MODULE_IN_NAMESPACE(WorldScriptModule, MMO)
