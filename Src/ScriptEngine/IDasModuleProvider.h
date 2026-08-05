#pragma once

#include "daScript/ast/ast.h"
#include "daScript/simulate/simulate.h"
#include <memory>

namespace MMO
{
    class IDasLangModuleProvider
    {
    public:
        virtual ~IDasLangModuleProvider() = default;

        /**
         * @brief 注册服务专用模块（PULL_MODULE 触发 REGISTER_MODULE 自注册）
         *
         * 官方范式：宿主在 PULL_ALL_DEFAULT_MODULES 之后、Module::Initialize 之前，
         * 拉取各服务专用模块（构造时注册绑定）。服务模块因依赖 builtin（构造函数里
         * lib.addBuiltInModule() 需 Module::require("$")），必须在默认模块注册后调用。
         */
        virtual void RegisterModules() = 0;

        /**
         * @brief 创建模块
         * @param group 模块组
         */
        virtual void CreateModules(das::ModuleGroup &group) = 0;

        /**
         * @brief swap后，需要把新的Context通知给各模块
         * @param ctx
         */
        virtual void OnContextSwapped(std::shared_ptr<das::Context> ctx) = 0;

        /**
         * @brief 每帧tick前，转发dt
         * @param dt 帧频率
         */
        virtual void OnPrevTick(float dt) = 0;

        /**
         * @brief swap前，要清空定时器回调
         */
        virtual void DrainTimers() = 0;

        /**
         * @brief 热重载前回调——提供者可在旧 Context 销毁前保存需要迁移的状态
         * @param oldCtx 旧 Context（即将被替换）
         */
        virtual void BeforeReload(std::shared_ptr<das::Context> oldCtx) = 0;

        /**
         * @brief 热重载后回调——提供者在新 Context 上恢复/重建状态
         * @param newCtx 新 Context（已 simulate + Rebind）
         */
        virtual void AfterReload(std::shared_ptr<das::Context> newCtx) = 0;
    };
} // namespace MMO