--- @file xmake.lua
--- @brief CommonLog — 日志服务

target("CommonLog")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("CommonCore", "Spdlog")
    add_defines("SPDLOG_FMT_EXTERNAL")
