#pragma once

#include "Common/ECS/EntityID.h"
#include "ScriptEngine/IDasHost.h"
#include "daScript/ast/ast.h"
#include "daScript/daScriptModule.h"
#include "daScript/misc/string_writer.h"
#include "daScript/simulate/simulate.h"
#include <memory>

// 脚本侧强类型 EntityID 值绑定（cast 特化——须在头文件，供所有 addExtern 绑定 TU 可见）
MAKE_TYPE_FACTORY(EntityID, MMO::ECS::EntityID)

namespace das
{
    // EntityID 是 uint64 包装，值走 vec4f 低 64 位（同 uint64 寄存器表示）
    template <>
    struct cast<MMO::ECS::EntityID>
    {
        // 64 位值：to 用 v_extract_xi64 取完整 uint64（v_extract_xi 只取低 32 位会截断 scene 位），
        // from 用 v_ldui_half（同官方 cast<uint64_t>：cast.h:336-339）
        static __forceinline MMO::ECS::EntityID to(vec4f x)
        {
            return MMO::ECS::EntityID(v_extract_xi64(v_cast_vec4i(x)));
        }

        static __forceinline vec4f from(MMO::ECS::EntityID x)
        {
            uint64_t raw = x.raw;
            return v_cast_vec4f(v_ldui_half(&raw));
        }
    };

    template <>
    struct WrapType<MMO::ECS::EntityID>
    {
        enum
        {
            value = true
        };

        typedef uint64_t type;
        typedef uint64_t rettype;
    };
} // namespace das

namespace MMO
{

    void LogInfo(const char *text, das::Context *ctx, das::LineInfoArg *at);
    void LogWarn(const char *text, das::Context *ctx, das::LineInfoArg *at);
    void LogError(const char *text, das::Context *ctx, das::LineInfoArg *at);

    // EntityID ↔ uint64 转换（脚本侧跨边界/日志格式化用——AOT 生成代码引用，须头文件声明）
    ECS::EntityID EntityFromUInt64(uint64 v);
    uint64        EntityToUInt64(ECS::EntityID eid);

    /**
     * @brief Common 服务通用 das 模块（Log + EMsgID 枚举）
     *
     * 官方范式（cpp_api.rst）：绑定在【构造函数】注册，配合 REGISTER_MODULE 自注册。
     * 模块实例由 daScriptEnvironment 全局链表管理（Module ctor 自动插入），
     * das::Module::Shutdown() 统一释放——宿主不再 unique_ptr 持有，
     * 消除 double-delete（unique_ptr reset + Shutdown 重复释放）。
     *
     * 宿主拉取：DECLARE_MODULE(DasCommonModule)（文件作用域）+ PULL_MODULE(DasCommonModule)
     * （namespace 内，须在 PULL_ALL_DEFAULT_MODULES 之后）。
     */
    class DasCommonModule : public das::Module
    {
    public:
        DasCommonModule();

        das::ModuleAotType aotRequire(das::TextWriter &tw) const override;
    };
} // namespace MMO