/**
 * @file MassiveModule.cpp
 * @brief MassiveModule 实现 — Phase 1 骨架
 *
 * Phase 1: 仅注册 LogInfo / LogWarn / LogError，
 *          验证 DasLang → C++ 函数调用链路。
 */

#include "MassiveModule.h"
#include "Common/Log/Log.h"

#include <daScript/simulate/aot.h>
#include <daScript/simulate/simulate.h>
#include <daScript/ast/ast_interop.h>

namespace MMO
{

    // ── Phase 1 桥接函数 ──

    /**
     * @brief 脚本侧日志输出 — INFO 级别
     * @param text    日志文本（daScript 编译期完成字符串插值）
     * @param context daScript 运行时上下文（调用者不可见，自动填充）
     * @param at      脚本调用位置（调用者不可见，自动填充）
     */
    static void LogInfo(const char *text, das::Context * /*context*/, das::LineInfoArg *at)
    {
        if (at && at->fileInfo)
        {
            Log::At(ELogLevel::Info, at->fileInfo->name.c_str(), static_cast<int>(at->line),
                    "{}", text ? text : "(null)");
        }
        else
        {
            Log::Info("{}", text ? text : "(null)");
        }
    }

    /**
     * @brief 脚本侧日志输出 — WARN 级别
     * @param text    日志文本
     * @param context daScript 运行时上下文（调用者不可见，自动填充）
     * @param at      脚本调用位置（调用者不可见，自动填充）
     */
    static void LogWarn(const char *text, das::Context * /*context*/, das::LineInfoArg *at)
    {
        if (at && at->fileInfo)
        {
            Log::At(ELogLevel::Warn, at->fileInfo->name.c_str(), static_cast<int>(at->line),
                    "{}", text ? text : "(null)");
        }
        else
        {
            Log::Warn("{}", text ? text : "(null)");
        }
    }

    /**
     * @brief 脚本侧日志输出 — ERROR 级别
     * @param text    日志文本
     * @param context daScript 运行时上下文（调用者不可见，自动填充）
     * @param at      脚本调用位置（调用者不可见，自动填充）
     */
    static void LogError(const char *text, das::Context * /*context*/, das::LineInfoArg *at)
    {
        if (at && at->fileInfo)
        {
            Log::At(ELogLevel::Error, at->fileInfo->name.c_str(), static_cast<int>(at->line),
                    "{}", text ? text : "(null)");
        }
        else
        {
            Log::Error("{}", text ? text : "(null)");
        }
    }

    // ── Module 构造 ──

    MassiveModule::MassiveModule()
        : das::Module("massive")
    {
        // addBuiltInModule 已由 BindFunctions 在外部调用；
        // ModuleGroup 会在 compileDaScript 时通过依赖分析确保 "$" 模块已注册。
    }

    MassiveModule::~MassiveModule() = default;

    /**
     * @brief 注册所有桥接函数到模块
     *
     * @note 在 addModule 到 ModuleGroup 后、compileDaScript 前调用。
     *       ModuleLibrary 提供给 ExternFunc 注册上下文。
     */
    void MassiveModule::BindFunctions()
    {
        // 确保 builtin 模块 ("$") 对脚本可见
        // addBuiltinDependency 将 "$" 作为公开依赖加入到 require 列表
        das::ModuleLibrary lib(this);
        lib.addBuiltInModule();
        auto *builtin = das::Module::require("$");
        if (builtin)
        {
            addBuiltinDependency(lib, builtin, true);
        }

        // Phase 1: 仅注册日志函数
        // context / at 参数对脚本不可见（daScript 自动填充调用位置信息）
        addExtern<DAS_BIND_FUN(LogInfo)>(*this, lib, "LogInfo",
                                         das::SideEffects::modifyExternal)
            ->args({"text", "context", "at"});
        addExtern<DAS_BIND_FUN(LogWarn)>(*this, lib, "LogWarn",
                                         das::SideEffects::modifyExternal)
            ->args({"text", "context", "at"});
        addExtern<DAS_BIND_FUN(LogError)>(*this, lib, "LogError",
                                          das::SideEffects::modifyExternal)
            ->args({"text", "context", "at"});

        // Phase 2: 在此追加其余 15 个桥接函数
    }

} // namespace MMO
