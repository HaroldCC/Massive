--- @file xmake.lua
--- @brief CommonCore — 基础核心库

target("CommonCore")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("tracy", {public = true})
