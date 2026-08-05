#pragma once

#include "Common/Core/Types.h"

namespace MMO
{
    /**
     * @brief 3D 世界坐标（C++ 高频组件，EnTT SoA 存储）
     *
     * 复制用整数坐标（float32 × 100 → int32），见 Replicate.proto PositionDelta。
     */
    struct Position
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };
} // namespace MMO