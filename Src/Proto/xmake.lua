--- @file xmake.lua
--- @brief Proto — 协议库（GenMsgID + protoc 代码生成）
---
--- 构建流程（增量）:
---   1. proto_msgid rule: 业务 *.proto 变化 → GenMsgID.py 增量更新 MsgID.proto
---   2. proto_gen rule: *.proto 变化 → protoc 生成 AutoGen/*.pb.{h,cc}
---   3. 编译 AutoGen/*.pb.cc → Proto 静态库

--- rule: 扫描业务 proto，增量生成 MsgID.proto（在 protoc 之前执行一次）
rule("proto_msgid")
    on_config(function (target)
        local protoDir = path.join(os.projectdir(), "Src/Proto")
        local genScript = path.join(os.projectdir(), "Tools/Proto/GenMsgID.py")
        local msgidProto = path.join(protoDir, "MsgID.proto")

        -- 增量：业务 proto 比 MsgID.proto 新才重新生成
        local depfiles = os.files(path.join(protoDir, "*.proto"))
        local dirty = not os.isfile(msgidProto)
        if not dirty then
            local msgidMtime = os.mtime(msgidProto)
            for _, f in ipairs(depfiles) do
                if path.filename(f) ~= "MsgID.proto" and os.mtime(f) > msgidMtime then
                    dirty = true
                    break
                end
            end
        end

        if dirty then
            os.vrunv("python", {genScript, "--proto-dir", protoDir, "--output", msgidProto})
            cprint("${color.success}[proto] MsgID.proto 已更新")
        end
    end)

--- rule: protoc 生成 C++ 代码（增量，每个 .proto 文件）
rule("proto_gen")
    set_extensions(".proto")

    on_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        import("core.project.depend")

        local protoc = path.join(os.projectdir(), "ThirdParty/Bin/protobuf/bin/protoc.exe")
        local protoDir = path.join(os.projectdir(), "Src/Proto")
        local autogenDir = path.join(os.projectdir(), "Src/Proto/AutoGen")

        local basename = path.basename(sourcefile)
        local pbcc = path.join(autogenDir, basename .. ".pb.cc")

        -- protoc 要求传入的文件名以 --proto_path 为精确前缀，传文件名 + 在 protoDir 下运行
        local protoFile = path.filename(sourcefile)

        batchcmds:mkdir(autogenDir)
        batchcmds:show_progress(opt.progress, "${color.build.object}proto.gen %s", sourcefile)
        batchcmds:vrunv(protoc, {
            "--proto_path=" .. protoDir,
            "--cpp_out=" .. autogenDir,
            protoFile
        }, {curdir = protoDir})

        -- 编译生成的 .pb.cc
        local objfile = target:objectfile(pbcc)
        table.insert(target:objectfiles(), objfile)
        batchcmds:compile(pbcc, objfile)

        -- 增量依赖
        batchcmds:add_depfiles(sourcefile)
        batchcmds:set_depmtime(os.mtime(objfile))
        batchcmds:set_depcache(target:dependfile(objfile))
    end)

target("Proto")
    set_kind("static")
    set_warnings("none")  -- protobuf 生成代码不参与项目警告策略

    --- MsgID.proto 必须在其它 proto 之前生成
    add_rules("proto_msgid", "proto_gen")

    --- 业务 proto（MsgID.proto 由 proto_msgid 生成后一并被 proto_gen 处理）
    add_files("*.proto")

    add_deps("Protobuf")
    add_includedirs("$(projectdir)/Src/Proto/AutoGen", {public = true})
