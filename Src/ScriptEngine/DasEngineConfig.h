#pragma once

#include "Common/Core/Types.h"
#include <string>

namespace MMO
{
    enum class EScriptMode
    {
        Develop, // 源码编译 + 热重载 + debugger
        Release, // AOT优先 + .dasbin缓存/补丁
    };

    struct DasLangEngineConfig
    {
        std::string dasLangRoot   = "Script"; // 根目录
        std::string projectFile   = "";       // .das_project 项目文件路径（相对项目根，为空不启用）
        std::string dasbinDir     = "";       // .dasbin目录(为空不缓存)
        std::string patchDir      = "";       // 补丁目录
        std::string dasbinKeyHex  = "";       // .dasbin AES密钥（hex 为空不加密）
        EScriptMode mode          = EScriptMode::Develop;
        bool        enableWatcher = true;
        int64       watchPollMs   = 500;
    };
} // namespace MMO