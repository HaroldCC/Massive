--- @file tracy.lua
--- @brief Tracy — 实时性能分析器（内联编译）

target("tracy")
    set_kind("static")
    set_warnings("none")
    add_rules("Rules.ThirdParty")
    add_files("tracy/public/TracyClient.cpp")
    add_sysincludedirs("$(projectdir)/ThirdParty/tracy/public", {public = true})
    add_defines("TRACY_ENABLE")
