#pragma once

#include "ScriptEngine/IDasModuleProvider.h"

namespace MMO
{

    /**
     * @brief World 服务 das 模块提供者（IDasLangModuleProvider 实现）
     *
     * 官方范式：模块经 REGISTER_MODULE 自注册进 daScriptEnvironment 全局链表，
     * 宿主 PULL_MODULE(WorldScriptModule) 拉取一次。CreateModules 用
     * Module::require("world") 获取实例——不再每次 new（避免 "Module 'world'
     * already created" DAS_FATAL_ERROR）也不再 unique_ptr 持有（避免 double-delete）。
     */
    class WorldDasModule : public IDasLangModuleProvider
    {
    public:
        WorldDasModule()           = default;
        ~WorldDasModule() override = default;

        /**
         * @brief 注册服务专用模块（PULL_MODULE(WorldScriptModule)）
         */
        void RegisterModules() override;

        /**
         * @brief 创建模块
         * @param group 模块组
         */
        void CreateModules(das::ModuleGroup &group) override;

        /**
         * @brief swap后，需要把新的Context通知给各模块
         * @param ctx
         */
        void OnContextSwapped(std::shared_ptr<das::Context> ctx) override;

        /**
         * @brief 每帧tick前，转发dt
         * @param dt 帧频率
         */
        void OnPrevTick(float dt) override;

        /**
         * @brief swap前，要清空定时器回调
         */
        void DrainTimers() override;

        /**
         * @brief 热重载前回调（保存跨 Context 状态）
         */
        void BeforeReload(std::shared_ptr<das::Context> oldCtx) override;

        /**
         * @brief 热重载后回调（恢复跨 Context 状态）
         */
        void AfterReload(std::shared_ptr<das::Context> newCtx) override;
    };
} // namespace MMO