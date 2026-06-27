--- @file xmake.lua
--- @brief WorldServer — 场景世界服务器进程

target("WorldServer")
    set_kind("binary")
    add_files("*.cpp")
    add_files("Handler/*.cpp")
    add_headerfiles("*.h")
    add_deps(
        "CommonCore",
        "CommonDB",
        "CommonNetwork",
        "CommonQueue",
        "CommonCrypto",
        "CommonECS",
        "CommonTimer",
        "CommonConfig",
        "CommonLog",
        "Proto"
    )
    add_deps("asio", {public = true})
