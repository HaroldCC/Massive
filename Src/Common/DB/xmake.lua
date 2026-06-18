--- @file xmake.lua
--- @brief CommonDB — 数据库层（DBWorkerPool + DBRange + Column）

target("CommonDB")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_headerfiles("AutoGen/*.gen.h")
    add_deps("CommonCore", "CommonQueue", "CommonLog", "LibPQ")
