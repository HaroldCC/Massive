#pragma once

#include "daScript/ast/ast.h"
#include "daScript/daScriptModule.h"
#include "daScript/misc/string_writer.h"

namespace MMO
{

    /**
     * @brief World 服务专用 daScript 模块
     * @note 脚本侧 `require world` 获取该模块提供的绑定（消息类型 + EMsgID 枚举）。
     *       绑定内容由生成器产出（ProtoBindIndex.gen.h 的 RegisterAllProtoMessageTypes）。
     *       独立成文件：WorldServer 与 AotGen（AOT 生成宿主）共用同一模块注册路径，
     *       保证 aotHash 一致（生成端 = 运行端模块集）。
     */
    class WorldScriptModule : public das::Module
    {
    public:
        WorldScriptModule();
        ~WorldScriptModule() override = default;

        das::ModuleAotType aotRequire(das::TextWriter &tw) const override;

        void Build();
    };

} // namespace MMO
