#include "WorldScriptModule.h"

#include "World/AutoGen/ProtoBindIndex.gen.h"

namespace MMO
{

    WorldScriptModule::WorldScriptModule() : das::Module("world")
    {
    }

    void WorldScriptModule::Build()
    {
        das::ModuleLibrary lib(this);
        lib.addBuiltInModule();
        // 注册生成器产出的全部消息类型 + EMsgID 枚举（Common/World 消息）
        RegisterAllProtoMessageTypes(*this, lib);
    }

    das::ModuleAotType WorldScriptModule::aotRequire(das::TextWriter &tw) const
    {
        // AOT 生成端需要看到 World 侧绑定头（注册函数声明）：
        // 生成的 .das.cpp 会引用 RegisterAllProtoMessageTypes 注册的类型与枚举。
        tw << "#include \"World/AutoGen/ProtoBindIndex.gen.h\"\n";
        return das::ModuleAotType::cpp;
    }

} // namespace MMO
