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
         * @brief 入口脚本文件
         * @return const char* 文件名
         */
        virtual const char *MainScriptFile() const = 0;

        /**
         * @brief 模块名字
         * @return const char* 模块名字
         */
        virtual const char *ModuleName() const = 0;
    };
} // namespace MMO