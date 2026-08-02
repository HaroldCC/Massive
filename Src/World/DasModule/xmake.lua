--- @file xmake.lua
--- @brief ProtoScriptModule — 服务侧 Proto 绑定库（World 服务的消息类型 das 模块）
---
--- 职责：把 World 服务的消息绑定（WorldScriptModule + 生成器产出的 *.gen.cpp）
---       封装成独立静态库，供 WorldServer（运行期）与 AotGen（生成期）共同依赖。
---       两端链接同一库 → aotHash 一致（生成端 = 运行端模块集）。
---
--- 依赖：ScriptEngine（DasCommonModule/libDaScript）+ Proto（.pb.h/.pb.cc）
--- 生成：*.gen.cpp 由 gen_msg_bindings rule（本 target 挂载）在 on_load 阶段生成

-- @note 用 on_load（而非 on_config）：add_files 的通配符在 target 描述解析阶段
--       就已经固化成 sourcebatch，on_config 执行时新落盘的 .gen.cpp 不会被
--       重新纳入编译——干净 checkout 后首次构建必然缺少 RegisterAllProtoMessageTypes
--       等符号导致链接失败。on_load 运行更早，此时显式 target:add("files", ...)
--       才能让刚生成的文件进入本次构建的 sourcebatch。
rule("gen_msg_bindings")
    on_load(function (target)
        local service    = target:extraconf("rules", "gen_msg_bindings", "service") or "world"
        local protoDir   = path.join(os.projectdir(), "Src/Proto")
        local autogenDir = path.join(os.projectdir(), "Src/World/AutoGen")
        local genScript  = path.join(os.projectdir(), "Tools/Script/GenMsgBindings.py")

        local indexFile = path.join(autogenDir, "ProtoBindIndex.gen.cpp")
        local dirty = not os.isfile(indexFile)
        if not dirty then
            local indexMtime = os.mtime(indexFile)
            -- 公共 proto + 服务子目录 proto 都纳入变更检测
            local watchFiles = os.files(path.join(protoDir, "*.proto"))
            local svcDir = path.join(protoDir, service:gsub("^%l", string.upper))
            if os.isdir(svcDir) then
                table.join2(watchFiles, os.files(path.join(svcDir, "*.proto")))
            end
            for _, f in ipairs(watchFiles) do
                if os.mtime(f) > indexMtime then
                    dirty = true
                    break
                end
            end
        end
        if dirty then
            os.vrunv("python", {genScript, "--service", service,
                                "--proto-dir", protoDir, "--cpp-out", autogenDir})
            cprint("${color.success}[msgbind] handler bindings 已更新")
        end

        for _, f in ipairs(os.files(path.join(autogenDir, "*.gen.cpp"))) do
            target:add("files", f)
        end
    end)

target("ProtoScriptModule")
    set_kind("static")
    add_headerfiles("*.h")
    add_files("WorldScriptModule.cpp")
    add_includedirs(
        "$(projectdir)/Src",
        "$(projectdir)/Src/Proto/AutoGen"
    )
    add_deps("ScriptEngine", "Proto", {public = true})
    -- 生成 World 服务的消息绑定（ProtoBindIndex.gen.* 等）并纳入编译
    add_rules("gen_msg_bindings", {service = "world"})

