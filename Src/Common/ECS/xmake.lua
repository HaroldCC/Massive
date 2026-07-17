--- @file xmake.lua
--- @brief CommonECS — ECS 框架

target("CommonECS")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("CommonCore", "CommonLog")
    add_deps("entt", {public = true})
    add_deps("libDaScript", {public = true})
