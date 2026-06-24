--- @file xmake.lua
--- @brief CommonConfig — 配置加载

target("CommonConfig")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("CommonCore")
    add_deps("tomlplusplus", {public = true})
