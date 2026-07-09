--- @file xmake.lua
--- @brief TestClient — 多客户端压力测试工具

target("TestClient")
    set_kind("binary")
    add_files("*.cpp")
    add_files("Scenarios/*.cpp")
    add_headerfiles("*.h")
    add_headerfiles("Scenarios/*.h")
    add_deps("CommonCore", "CommonCrypto", "CommonNetwork", "CommonLog", "Proto", "CommonConfig")
    add_deps("tomlplusplus", {public = true})
    set_runargs("--config Config/testclient.toml")
