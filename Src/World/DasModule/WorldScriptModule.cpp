#include "WorldScriptModule.h"

#include "World/AutoGen/GameEventBindings.gen.h"
#include "World/AutoGen/ProtoBindIndex.gen.h"
#include "World/DasModule/WorldBridge.h"

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
    }

    das::ModuleAotType WorldScriptModule::aotRequire(das::TextWriter &tw) const
    {
        // AOT 生成端需要看到 World 侧绑定头（注册函数声明）：
        // 生成的 .das.cpp 会引用 RegisterAllProtoMessageTypes 注册的类型与枚举。
        tw << "#include \"World/AutoGen/ProtoBindIndex.gen.h\"\n";
        // 事件类型（GameEventEnvelope / *Event / EGameEventType）
        tw << "#include \"World/AutoGen/GameEventBindings.gen.h\"\n";
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
