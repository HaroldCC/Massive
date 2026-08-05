#pragma once

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 生命值
     *
     * 战斗系统写入（含脚本经 Bridge 写入），复制系统读。
     */
    struct Health
    {
        int32 current = 0;
        int32 max     = 0;
    };

} // namespace MMO