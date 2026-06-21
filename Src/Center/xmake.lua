--- @file xmake.lua
--- @brief CenterServer — 中心服务器进程

target("CenterServer")
    set_kind("binary")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("CommonCore", "CommonNetwork", "CommonLog", "Proto", "CommonConfig")
