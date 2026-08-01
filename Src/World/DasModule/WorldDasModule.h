#include "ScriptEngine/IDasModuleProvider.h"

namespace MMO
{

    class WorldDasModule : public IDasLangModuleProvider
    {
    public:
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
         * @brief 入口脚本文件
         * @return const char* 文件名
         */
        const char *MainScriptFile() const override;

        /**
         * @brief 模块名字
         * @return const char* 模块名字
         */
        const char *ModuleName() const override;
    };
} // namespace MMO