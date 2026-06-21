--- @file xmake.lua
--- @brief CommonNetwork — 网络层（IOContextPool + CryptoSession + PacketHeader）

target("CommonNetwork")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("CommonCore", "CommonCrypto", "Asio", "CommonQueue")
