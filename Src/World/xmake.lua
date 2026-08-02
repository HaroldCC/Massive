--- @file xmake.lua
--- @brief WorldServer — 场景世界服务器进程

-- @note 消息绑定生成（gen_msg_bindings rule）已移至 ProtoScriptModule
--       （Src/World/DasModule/xmake.lua），保证 .gen.cpp 先于本 target 编译就绪。
--       AOT 生成（Rules.das_aot）仅 Release 生效。

target("WorldServer")
    set_kind("binary")
    if is_mode("release") then
        -- AOT：仅 Release 把脚本转原生（AotGen 已由 das_aot rule 依赖构建）
        add_rules("Rules.das_aot", {service = "world", entry = "World/main.das"})
        add_includedirs("$(projectdir)/Src")  -- 生成的 .das.cpp include 各模块 aotRequire 发射的头
    end
    -- 排除：*.gen.cpp（生成产物）与 WorldScriptModule.cpp（二者归 ProtoScriptModule 库），
    --       WorldDasModule.cpp（Provider）保留在本 target。
    add_files("**.cpp", {excludes = {
        "DasModule/WorldScriptModule.cpp",
        "AutoGen/*.gen.cpp"
    }})
    add_headerfiles("*.h")
    add_deps(
        "CommonCore",
        "CommonDB",
        "CommonNetwork",
        "CommonQueue",
        "CommonCrypto",
        "CommonECS",
        "CommonTimer",
        "CommonConfig",
        "CommonLog",
        "Proto",
        "ScriptEngine",
        "ProtoScriptModule"
    )
    if is_mode("release") then
        add_deps("AotGen")   -- das_aot rule 内 target:dep("AotGen") 需要显式依赖
    end
    add_deps("asio", {public = true})
