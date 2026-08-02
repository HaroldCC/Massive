#include "WorldDasModule.h"

#include "World/DasModule/WorldScriptModule.h"

namespace MMO
{

    void WorldDasModule::CreateModules(das::ModuleGroup &group)
    {
        // 服务专用模块：脚本 `require world` 拿到的消息类型 / EMsgID 枚举绑定。
        auto worldModule = std::make_unique<WorldScriptModule>();
        worldModule->Build();
        group.addModule(worldModule.release());
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

    const char *WorldDasModule::MainScriptFile() const
    {
        return "World/main.das";
    }

    const char *WorldDasModule::ModuleName() const
    {
        return "world";
    }

} // namespace MMO
