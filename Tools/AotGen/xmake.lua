--- @file xmake.lua
--- @brief AotGen — 项目自建 AOT 生成宿主（构建期工具）
---
--- 作用：以与运行端相同的模块集（Common + world）驱动 daslib aot()，
---       把各服务入口脚本（及其 require 闭包）转成 .das.cpp 编进宿主。
--- 依赖：ScriptEngine（DasCommonModule）+ ProtoScriptModule（World 侧消息绑定）。
---       WorldServer 与 AotGen 都链接 ProtoScriptModule → aotHash 两端一致。

target("AotGen")
    set_kind("binary")
    add_files("*.cpp")
    add_deps(
        "ScriptEngine",        -- DasCommonModule + libDaScript
        "ProtoScriptModule",   -- World 服务消息绑定（WorldScriptModule + .gen.cpp）
        "CommonCore",
        "CommonLog",
        "CommonTimer",
        "CommonCrypto",        -- DasLangSerializer 依赖 Aes256Gcm
        "Proto"                -- .pb.h/.pb.cc
    )
    add_includedirs(
        "$(projectdir)/Src",
        "$(projectdir)/Src/Proto/AutoGen"
    )
    if is_plat("windows") then
        add_syslinks("dbghelp", "ws2_32")
    elseif is_plat("linux") then
        add_syslinks("dl", "pthread")
    end

    set_policy("build.fence", true)