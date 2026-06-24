--- @file xmake.lua
--- @brief CommonCrypto — 加解密库

target("CommonCrypto")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("CommonCore")
    add_deps("openssl", {public = true})
