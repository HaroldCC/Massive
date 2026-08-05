#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 3D 移动速度（units/s）
     *
     * Movement 系统写入，Movement 阶段消费。脚本只读。
     */
    struct Velocity
    {
        float vx = 0.0f;
        float vy = 0.0f;
        float vz = 0.0f;
    };

} // namespace MMO