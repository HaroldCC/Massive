--- @file xmake.lua
--- @brief SocialServer — 社交服务器进程（stub）

target("SocialServer")
    set_kind("binary")
    set_default(false)
    add_deps("CommonCore", "CommonDB", "CommonNetwork", "CommonLog", "Proto")
