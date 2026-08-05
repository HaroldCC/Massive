#pragma once

#include "Common/ECS/EntityID.h"
#include "Common/ECS/Grid.h"

#include "entt/entt.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MMO
{

    /**
     * @brief AOI 增量更新（函数式）
     *
     * 流程：
     *   1. 遍历有 Position + Velocity 的实体 → Grid.Update（跨格才动）
     *   2. 对每个玩家（Position + PlayerConn）→ QueryRadius → 精确 XZ/Y 过滤
     *   3. 与上帧快照做差 → enter/leave 事件 → 写入 _aoiEvents
     *   4. 更新快照
     *
     * 所有 index 均为 EntityIndex（EnTT 内部索引），不是对外 EntityID。
     *
     * @param reg      EnTT registry
     * @param grid     空间索引
     * @param prevState  上帧玩家可见集（输入输出）
     * @param outEnter  本帧 enter 事件（observerIdx, entityIdx）
     * @param outLeave  本帧 leave 事件
     */
    void SystemAOI(entt::registry                                                             &reg,
                   ECS::Grid                                                                  &grid,
                   std::unordered_map<ECS::EntityIndex, std::unordered_set<ECS::EntityIndex>> &prevState,
                   std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>>                 &outEnter,
                   std::vector<std::pair<ECS::EntityIndex, ECS::EntityIndex>>                 &outLeave);

} // namespace MMO