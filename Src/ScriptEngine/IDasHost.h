#pragma once

#include "daScript/simulate/simulate.h"
#include "Common/Core/Types.h"

namespace MMO
{

    class GameEventBus;

    namespace ECS
    {
        class Scene;
    } // namespace ECS

    class IDasLangHost
    {
    public:
        virtual ~IDasLangHost() = default;

        virtual void SendRawToClient(uint32 sessionID, uint32 msgID, const uint8 *data, size_t len) = 0;

        /**
         * @brief 获取宿主持有的游戏事件总线（ECS_06）
         * @return 事件总线；宿主未实现返回 nullptr（脚本事件派发静默跳过）
         */
        virtual GameEventBus *GetGameEventBus() = 0;

        /**
         * @brief 获取默认场景（Bridge_* 实体操作的目标）
         * @return 默认场景；无返回 nullptr
         */
        virtual ECS::Scene *GetDefaultScene() = 0;

        /**
         * @brief 按 EntityID 定位场景（跨场景查询）
         * @param entityID 实体 ID
         * @return 场景；无效返回 nullptr
         */
        virtual ECS::Scene *GetSceneByEntityID(uint64 entityID) = 0;
    };
} // namespace MMO