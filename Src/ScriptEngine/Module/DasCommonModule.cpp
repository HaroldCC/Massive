#include "DasCommonModule.h"
#include "Common/Log/Log.h"
#include "Proto/AutoGen/EMsgIDBind.gen.h"
#include "daScript/ast/ast.h"
#include "daScript/daScript.h"
#include "daScript/simulate/debug_info.h"
#include "daScript/simulate/simulate.h"

namespace MMO
{

    /**
     * @brief 脚本侧 EntityID 值类型注解（ManagedValueAnnotation 范式）
     *
     * isLocal=true → 脚本 `let eid = ...` / `var eid` 无需 unsafe（POD 值语义）。
     * 脚本持有 EntityID 值拷贝（铁律 1：组件数据仍在 C++/EnTT，此仅为 ID 值）。
     */
    struct EntityIDAnnotation final : das::ManagedValueAnnotation<ECS::EntityID>
    {
        EntityIDAnnotation(das::ModuleLibrary &ml)
            : das::ManagedValueAnnotation<ECS::EntityID>(ml, "EntityID", "MMO::ECS::EntityID")
        {
        }

        virtual void walk(das::DataWalker &walker, void *data) override
        {
            if (!walker.reading)
            {
                auto    *t = static_cast<const ECS::EntityID *>(data);
                uint64_t v = t->raw;
                walker.UInt64(v);
            }
        }

        virtual bool isLocal() const override
        {
            return true;
        }

        virtual bool hasNonTrivialCtor() const override
        {
            return false;
        }

        virtual bool canBePlacedInContainer() const override
        {
            return true;
        }
    };

    // EntityID ↔ uint64 转换辅助（脚本侧跨边界用——EntityID 值类型无默认 uint64 转换）
    ECS::EntityID EntityFromUInt64(uint64 v)
    {
        return ECS::EntityID(v);
    }

    uint64 EntityToUInt64(ECS::EntityID eid)
    {
        return eid.raw;
    }

    void LogInfo(const char *text, das::Context *ctx, das::LineInfoArg *at)
    {
        if (nullptr != at && nullptr != at->fileInfo)
        {
            Log::At(ELogLevel::Info,
                    at->fileInfo->name.c_str(),
                    at->line,
                    "{}",
                    nullptr != text ? text : "(null)");
        }
        else
        {
            Log::Info("{}", nullptr != text ? text : "(null)");
        }
    }

    void LogWarn(const char *text, das::Context *ctx, das::LineInfoArg *at)
    {
        if (nullptr != at && nullptr != at->fileInfo)
        {
            Log::At(ELogLevel::Warn,
                    at->fileInfo->name.c_str(),
                    at->line,
                    "{}",
                    nullptr != text ? text : "(null)");
        }
        else
        {
            Log::Warn("{}", nullptr != text ? text : "(null)");
        }
    }

    void LogError(const char *text, das::Context *ctx, das::LineInfoArg *at)
    {
        if (nullptr != at && nullptr != at->fileInfo)
        {
            Log::At(ELogLevel::Error,
                    at->fileInfo->name.c_str(),
                    at->line,
                    "{}",
                    nullptr != text ? text : "(null)");
        }
        else
        {
            Log::Error("{}", nullptr != text ? text : "(null)");
        }
    }

    DasCommonModule::DasCommonModule() : das::Module("Common")
    {
        das::ModuleLibrary lib(this);
        lib.addBuiltInModule();

        // Log —— ctx/at 是隐藏 interop 参数（脚本侧不可见），必须用约定名 "context"/"at"
        das::addExtern<DAS_BIND_FUN(LogInfo)>(*this,
                                              lib,
                                              "LogInfo",
                                              das::SideEffects::modifyExternal,
                                              "MMO::LogInfo")
            ->args({"text", "context", "at"});
        das::addExtern<DAS_BIND_FUN(LogWarn)>(*this,
                                              lib,
                                              "LogWarn",
                                              das::SideEffects::modifyExternal,
                                              "MMO::LogWarn")
            ->args({"text", "context", "at"});
        das::addExtern<DAS_BIND_FUN(LogError)>(*this,
                                               lib,
                                               "LogError",
                                               das::SideEffects::modifyExternal,
                                               "MMO::LogError")
            ->args({"text", "context", "at"});

        // EMsgID 枚举绑定——全局共享 ID 值空间，所有服务 require Common 即可引用
        RegisterEMsgIDEnumeration(*this);

        // 脚本侧强类型 EntityID（ManagedValueAnnotation 值类型）——脚本拿 EntityID 而非裸 uint64，
        // 与 sessionID/accountID 强区分（官方 dasUnitTest EntityId 同款范式）
        addAnnotation(new EntityIDAnnotation(lib));

        // EntityID ↔ uint64 显式转换（跨边界/日志格式化用——值类型无默认 uint64 转换）
        das::addExtern<DAS_BIND_FUN(EntityFromUInt64)>(*this,
                                                       lib,
                                                       "EntityFromUInt64",
                                                       das::SideEffects::none,
                                                       "MMO::EntityFromUInt64")
            ->args({"v"});
        das::addExtern<DAS_BIND_FUN(EntityToUInt64)>(*this,
                                                     lib,
                                                     "EntityToUInt64",
                                                     das::SideEffects::none,
                                                     "MMO::EntityToUInt64")
            ->args({"eid"});
    }

    das::ModuleAotType DasCommonModule::aotRequire(das::TextWriter &tw) const
    {
        tw << "#include \"ScriptEngine/Module/DasCommonModule.h\"\n";
        // EMsgID 外部枚举 cppName=MMO::Proto::EMsgID——AOT 生成代码引用它需要 MsgID.pb.h
        tw << "#include \"MsgID.pb.h\"\n";
        return das::ModuleAotType::cpp;
    }
} // namespace MMO

// 官方范式：REGISTER_MODULE 生成 register_DasCommonModule()（new + 注册进全局链表），
// 宿主 PULL_MODULE(DasCommonModule) 拉取。模块实例归 daScriptEnvironment 管理，
// das::Module::Shutdown() 统一释放——不再由宿主 unique_ptr 持有，消除 double-delete。
REGISTER_MODULE_IN_NAMESPACE(DasCommonModule, MMO)