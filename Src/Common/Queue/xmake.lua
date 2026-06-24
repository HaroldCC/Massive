--- @file xmake.lua
--- @brief CommonQueue — 线程安全队列

target("CommonQueue")
    set_kind("headeronly")
    add_headerfiles("*.h")
    add_deps("CommonCore")
    add_deps("concurrentqueue", {public = true})
