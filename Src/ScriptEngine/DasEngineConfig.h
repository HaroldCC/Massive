#pragma once

#include <string>

namespace MMO
{
    struct DasLangEngineConfig
    {
        std::string dasLangRoot    = "Script"; // 根目录
        bool        enableDebugger = true;     // 调试开关
    };
} // namespace MMO