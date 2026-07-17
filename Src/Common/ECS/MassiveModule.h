/**
 * @file MassiveModule.h
 * @brief DasLang C++ Module — 脚本层桥接入口
 *
 * 暴露 ~18 个窄接口函数给 DasLang 脚本，覆盖空间查询、属性查询、Tag 判断、
 * 世界交互、定时器、工具函数。脚本通过此 Module 与 C++ EnTT 物理层交互。
 *
 * Phase 1: 仅 log_info + log_warn + log_error 三个函数，验证编译链路。
 * Phase 2: 扩展完整 18 函数。
 */
#pragma once

#include <daScript/das_common.h>
#include <daScript/ast/ast.h>
#include <daScript/daScriptModule.h>

namespace MMO
{

    /**
     * @brief DasLang Module "massive" — 脚本与 C++ 的桥接通道
     */
    class MassiveModule : public das::Module
    {
    public:
        MassiveModule();
        ~MassiveModule() override;

        /**
         * @brief 注册所有桥接函数
         *
         * 在 addModule 到 ModuleGroup 后、compileDaScript 前调用。
         */
        void BindFunctions();
    };

} // namespace MMO
