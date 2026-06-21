--- @file xmake.lua
--- @brief LoginServer — 登录服务器进程

target("LoginServer")
    set_kind("binary")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("CommonCore", "CommonDB", "CommonCrypto", "CommonNetwork", "Proto", "CommonConfig")
