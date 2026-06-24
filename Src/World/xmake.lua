--- @file xmake.lua
--- @brief WorldServer — 场景世界服务器进程（stub）

target("WorldServer")
    set_kind("binary")
    set_default(false)
    add_deps(
        "CommonCore",
        "CommonDB",
        "CommonNetwork",
        "CommonQueue",
        "CommonCrypto",
        "CommonECS",
        "CommonTimer",
        "CommonLog",
        "Proto"
    )
