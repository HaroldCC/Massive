--- @file xmake.lua
--- @brief GateServer — 网关服务器进程（stub）

target("GateServer")
    set_kind("binary")
    set_default(false)
    add_deps("CommonCore", "CommonNetwork", "CommonQueue", "CommonCrypto", "Proto")
