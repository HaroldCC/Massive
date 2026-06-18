--- @file xmake.lua
--- @brief CommonTimer — 定时器服务

target("CommonTimer")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("CommonCore")
