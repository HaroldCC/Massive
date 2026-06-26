--- @file xmake.lua
--- @brief GateServer — 网关服务器进程（无状态连接代理）

target("GateServer")
    set_kind("binary")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("CommonCore", "CommonNetwork", "CommonQueue", "CommonCrypto", "CommonConfig", "CommonLog", "Proto")
