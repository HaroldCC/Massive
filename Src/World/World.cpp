#include "World.h"
#include "Common/ECS/Scene.h"
#include "StageScheduler.h"
#include "World/System/SystemAOI.h"
#include "World/System/SystemMovement.h"

#include <memory>

namespace MMO
{
    World::World(uint16 sceneID) : _sceneID(sceneID), _scene(std::make_unique<ECS::Scene>(sceneID))
    {
    }

    void World::Init()
    {
        // Movement 阶段：移动积分
        _stageScheduler.RegisterSystem(EStage::Movement, "SystemMovement", [this](float dt) {
            SystemMovement(_scene->GetEnttRegistry(), dt);
        });

        // AOI 阶段：增量可见集 + enter/leave 事件
        _stageScheduler.RegisterSystem(EStage::AOI, "SystemAOI", [this](float) {
            SystemAOI(_scene->GetEnttRegistry(), _scene->GetGrid(), _aoiState, _aoiEnters, _aoiLeaves);
        });

        // 其余阶段（SpatialIndex/EventDispatch/PostUpdate/Replicate）在后续接入
    }

    void World::Tick(float dtSeconds)
    {
        _stageScheduler.RunAllStage(dtSeconds);
    }
} // namespace MMO