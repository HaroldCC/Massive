--- @file xmake.lua
--- @brief CommonCore — 基础核心库

target("CommonCore")
    set_kind("static")
    add_headerfiles("*.h")
    add_deps("tracy", {public = true})
    add_deps("abseil", {public = true})

    add_files("*.cpp|Platform_*.cpp")

    -- 平台特有源文件
    -- 注意：MinGW 平台 is_plat("windows") 返回 false，须用 MMO_PLATFORM_WINDOWS 同款语义
    --       （_WIN32 宏），否则 Platform_Linux.cpp 会被误编进 Windows 构建。
    -- xmake 惯例：mingw 是独立平台名。用 is_plat("mingw", "windows") 显式列出。
    if is_plat("mingw", "windows") then
        add_files("Platform_Win.cpp")
        add_links("DbgHelp", {public = true})
    else
        add_files("Platform_Linux.cpp")

    end