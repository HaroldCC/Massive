#pragma once

#include "Common/Core/Types.h"
#include "Common/ECS/EntityID.h"
#include "StageScheduler.h"
#include "Common/ECS/Scene.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMO
{
    class World
    {
    public:
        explicit World(uint16 sceneID);

        World(const World &)            = delete;
        World &operator=(const World &) = delete;

        /**
         * @brief 初始化，创建场景+注册全部系统
         */
        void Init();

        void Tick(float dtSeconds);

        ECS::Scene &GetScene()
        {
            return *_scene;
        }

        const ECS::Scene &GetScene() const
        {
            return *_scene;
        }

        StageScheduler &GetStageScheduler()
        {
            return _stageScheduler;
        }

        uint16 GetSceneID() const
        {
            return _sceneID;
        }

        // ── AOI 状态（World 持有，供复制/脚本事件消费）──

        /** @brief 玩家可见集快照（observerIdx → 可见实体） */
        const std::unordered_map<ECS::EntityIndex, std::unordered_set<ECS::EntityIndex>> &GetAoiState() const
        {
            return _aoiState;
        }

        /** @brief 本帧 enter 事件（消费后清空） */
        std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>> &GetAoiEnters()
        {
            return _aoiEnters;
        }

        /** @brief 本帧 leave 事件（消费后清空） */
        std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>> &GetAoiLeaves()
        {
            return _aoiLeaves;
        }

    private:
        uint16                      _sceneID;
        std::unique_ptr<ECS::Scene> _scene;
        StageScheduler              _stageScheduler;

        // AOI 增量状态（SystemAOI 每帧更新）
        std::unordered_map<ECS::EntityIndex, std::unordered_set<ECS::EntityIndex>> _aoiState;
        std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>>                 _aoiEnters;
        std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>>                 _aoiLeaves;
    };
} // namespace MMO