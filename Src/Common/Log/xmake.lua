--- @file xmake.lua
--- @brief CommonLog — 日志服务
---
--- 依赖:
---   spdlog（异步日志 + stdout_color_sink + rotating_file_sink）
---   std::filesystem（目录创建，MinGW 需要显式链接）

target("CommonLog")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("CommonCore", "fmt")
    add_deps("spdlog", {public = true})
    add_defines("SPDLOG_FMT_EXTERNAL")

    --- std::filesystem（C++23 已内置到标准库，仅 MinGW 需显式链接）
    if is_plat("mingw") then
        add_syslinks("stdc++fs")
    end
