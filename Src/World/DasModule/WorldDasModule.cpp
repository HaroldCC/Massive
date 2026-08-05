#include "WorldDasModule.h"

#include "Common/Log/Log.h"
#include "World/DasModule/WorldScriptModule.h"

// 文件作用域（namespace 外）声明 register_WorldScriptModule()（PULL_MODULE 需要）
DECLARE_MODULE(WorldScriptModule);

namespace MMO
{

    void WorldDasModule::RegisterModules()
    {
        // 服务专用模块经 REGISTER_MODULE 自注册进 daScriptEnvironment 全局链表。
        // 引擎在 PULL_ALL_DEFAULT_MODULES 之后调用本方法（见 DasEngine::Initialize），
        // 之后 CreateModules 用 Module::require("world") 复用同一实例——
        // 不再每次 new（避免 DAS_FATAL_ERROR）也不 unique_ptr 持有（避免 double-delete）。
        PULL_MODULE(WorldScriptModule);
    }

    void WorldDasModule::CreateModules(das::ModuleGroup &group)
    {
        // 服务专用模块：脚本 `require world` 拿到的消息类型 / EMsgID 枚举绑定。
        // 官方范式：模块经 REGISTER_MODULE 自注册进 daScriptEnvironment 全局链表，
        // 宿主 PULL_MODULE(WorldScriptModule) 拉取一次。这里用 Module::require 获取实例，
        // 不 new 也不持有——热重载/多次编译复用同一实例（避免 "Module 'world' already
        // created" DAS_FATAL_ERROR），且无 double-delete。
        auto *worldModule = das::Module::require("world");
        if (nullptr == worldModule)
        {
            Log::Error(
                "WorldDasModule: world module not registered — PULL_MODULE(WorldScriptModule) missing?");
            return;
        }
        group.addModule(worldModule);
    }

    void WorldDasModule::OnContextSwapped(std::shared_ptr<das::Context> /*ctx*/)
    {
        // World 当前无跨 Context 缓存，留空
    }

    void WorldDasModule::OnPrevTick(float /*dt*/)
    {
        // World 当前无 tick 前回调，留空
    }

    void WorldDasModule::DrainTimers()
    {
        // World 当前无定时器回调，留空
    }

    void WorldDasModule::BeforeReload(std::shared_ptr<das::Context> /*oldCtx*/)
    {
        // World 当前无跨 Context 状态需在热重载前保存（实体数据在 C++/EnTT，脚本侧为值拷贝）。
        // 若将来脚本持有时钟/表状态需跨重载保留，在此序列化到持久 store，AfterReload 恢复。
    }

    void WorldDasModule::AfterReload(std::shared_ptr<das::Context> /*newCtx*/)
    {
        // 热重载后：新 Context 的 [init] 已由引擎执行，脚本全局重建。
        // World 无额外状态需恢复——留空。
    }

} // namespace MMO
