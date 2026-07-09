--- @file xmake.lua
--- @brief CommonCore — 基础核心库

target("CommonCore")
    set_kind("static")
    add_headerfiles("*.h")
    add_deps("tracy", {public = true})
    add_deps("abseil", {public = true})

    add_files("*.cpp|Platform_*.cpp")

    -- 平台特有源文件
    if is_plat("windows") then
        add_files("Platform_Win.cpp")
        add_links("DbgHelp", {public = true})
    else
        add_files("Platform_Linux.cpp")

    end