--- @file xmake.lua
--- @brief CommonNetwork — 网络层

target("CommonNetwork")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("CommonCore", "CommonCrypto", "CommonQueue")
    add_deps("asio", {public = true})
