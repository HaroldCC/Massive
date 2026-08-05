#pragma once

#include "Common/Core/Types.h"
#include "Common/ECS/EntityID.h"
#include "Common/ECS/Scene.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMO
{
    /**
     * @brief 单个玩家的复制打包（纯函数，线程安全）
     *
     * 只读 registry——模拟线程写完、打包线程只读。
     * 返回序列化字节（每玩家独立 buffer，无共享写）。
     *
     * 所有 index 均为 EntityIndex（EnTT 内部索引）。\n
     * EntityID 经 scene.ToEntityID(e) 获取（完整 64 位含 scene/version）。
     *
     * @param scene      场景（Scene::ToEntityID 用）
     * @param observerIdx 观察者 EnTT index
     * @param visible    该玩家可见实体集
     * @param enters     本帧 enter（(obsIdx, entIdx)）
     * @param leaves     本帧 leave
     * @return 序列化后的 EntityReplicateNtf 字节（空 = 无更新）
     */
    std::vector<uint8>
    PackPlayerReplicate(ECS::Scene                                                       &scene,
                        ECS::EntityIndex                                                  observerIdx,
                        const std::unordered_set<ECS::EntityIndex>                       &visible,
                        const std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>> &enters,
                        const std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>> &leaves);

    /**
     * @brief 复制调度器——串行入口，并行打包，统一发送
     *
     * 遍历玩家 → 投递 PackPlayerReplicate 到线程池 → join → 逐玩家发送。
     * 打包是纯只读（无共享写），发送串行（IO 层保证线程安全）。
     */
    class ReplicateScheduler
    {
    public:
        using SendFunc = std::function<void(uint32 sessionID, const uint8 *data, size_t len)>;

        ReplicateScheduler(ECS::Scene &scene, SendFunc sendFn, size_t workerCount = 4)
            : _scene(scene)
            , _sendFunc(std::move(sendFn))
            , _workerCount(workerCount)
        {
        }

        /**
         * @brief 每帧调用（模拟完成后）
         * @param aoiState 当前玩家可见集（AOI 输出）
         * @param enters   本帧 enter 事件
         * @param leaves   本帧 leave 事件
         */
        void
        Update(const std::unordered_map<ECS::EntityIndex, std::unordered_set<ECS::EntityIndex>> &aoiState,
               const std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>>                 &enters,
               const std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>>                 &leaves);

    private:
        ECS::Scene &_scene;
        SendFunc    _sendFunc;
        size_t      _workerCount;
    };
} // namespace MMO