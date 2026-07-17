/**
 * @file Position.h
 * @brief 实体 3D 位置 Component
 *
 * EnTT SoA 存储，swap_and_pop 删除策略（高频瞬态组件）。
 * 脚本只读不写——物理模拟由 C++ Movement System 更新。
 */
#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 3D 世界坐标
     *
     * C++ 高频组件，走 EnTT registry group 遍历。
     * swap_and_pop — 删除时无 tombstone 累积。
     */
    struct Position
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

} // namespace MMO
