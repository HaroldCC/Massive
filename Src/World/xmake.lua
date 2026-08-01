--- @file xmake.lua
--- @brief WorldServer — 场景世界服务器进程

-- @note 用 on_load（而非 on_config）：add_files 的通配符在 target 描述解析阶段
--       就已经固化成 sourcebatch，on_config 执行时新落盘的 .gen.cpp 不会被
--       重新纳入编译——干净 checkout 后首次构建必然缺少 RegisterAllProtoMessageTypes
--       等符号导致链接失败。on_load 运行更早，此时显式 target:add("files", ...)
--       才能让刚生成的文件进入本次构建的 sourcebatch。
rule("gen_msg_bindings")
    on_load(function (target)
        local protoDir   = path.join(os.projectdir(), "Src/Proto")
        local autogenDir = path.join(os.projectdir(), "Src/World/AutoGen")
        local genScript  = path.join(os.projectdir(), "Tools/Script/GenMsgBindings.py")

        local indexFile = path.join(autogenDir, "ProtoBindIndex.gen.cpp")
        local dirty = not os.isfile(indexFile)
        if not dirty then
            local indexMtime = os.mtime(indexFile)
            for _, f in ipairs(os.files(path.join(protoDir, "*.proto"))) do
                if os.mtime(f) > indexMtime then
                    dirty = true
                    break
                end
            end
        end
        if dirty then
            os.vrunv("python", {genScript, "--proto-dir", protoDir, "--cpp-out", autogenDir})
            cprint("${color.success}[msgbind] handler bindings 已更新")
        end

        for _, f in ipairs(os.files(path.join(autogenDir, "*.gen.cpp"))) do
            target:add("files", f)
        end
    end)

target("WorldServer")
    set_kind("binary")
    add_rules("gen_msg_bindings")
    add_files("**.cpp")
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
        "ScriptEngine"
    )
    add_deps("asio", {public = true})
