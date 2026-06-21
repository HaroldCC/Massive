--- @file xmake.lua
--- @brief Massive MMO Server — 顶层构建配置
--- 聚合 Src/ 所有子 target

set_project("Massive")
set_version("0.1.0")

set_languages("c++23")
add_rules("mode.debug", "mode.release")

--- MSVC: /utf-8 是强制要求 (fmt base.h 会 static_assert 检查)
if is_plat("windows") then
    add_cxflags("/utf-8")
end

--- MSVC 需要 /utf-8 标志，否则 fmt 等第三方会报 C2338
if is_plat("windows") then
    add_cxflags("/utf-8")
end

--- 全局警告策略: 项目代码全开 Warning
set_warnings("all")

add_rules("plugin.compile_commands.autoupdate", {outputdir = "$(projectdir)/build", lsp='clangd'})

set_targetdir(path.join("Bin", "$(plat)-$(arch)-$(mode)"))

--- 全局编译器标志
if is_mode("debug") then
    set_optimize("none")
    add_defines("MASSIVE_ENABLE_TRACY")
    set_symbols("debug")
end

if is_mode("release") then
    set_optimize("fastest")
    set_symbols("hidden")
end

--- 项目源码全局头文件路径
add_includedirs("Src")

--- ThirdParty 依赖统一声明
includes("ThirdParty/xmake.lua")

--- 聚合 Common 子 target
includes("Src/Common/xmake.lua")

--- 聚合各服务器进程
includes("Src/Login/xmake.lua")
includes("Src/Gate/xmake.lua")
includes("Src/World/xmake.lua")
includes("Src/Center/xmake.lua")
includes("Src/Social/xmake.lua")

--- 聚合 Proto
includes("Src/Proto/xmake.lua")
