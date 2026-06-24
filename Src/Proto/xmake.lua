--- @file xmake.lua
--- @brief Proto — 协议库（GenMsgID + protoc 代码生成）
---
---
--- 构建时序 (build.fence + add_deps):
---   1. protoc target 全量编译（含链接 protoc.exe）
---   2. Proto build 阶段开始
---      a. on_preparecmd_file: 注册 .obj（pb.cc 尚未生成）
---      b. on_buildcmd_file: 运行 protoc 生成 .pb.{h,cc} → 编译 .pb.cc

rule("proto_msgid")
    on_config(function (target)
        local protoDir   = path.join(os.projectdir(), "Src/Proto")
        local genScript  = path.join(os.projectdir(), "Tools/Proto/GenMsgID.py")
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

--- rule: 仿照 xmake 内置 protobuf.cpp 的两阶段模型
rule("proto_gen")
    set_extensions(".proto")

    add_deps("protoc")

    -- 阶段 1: 预注册 .pb.cc 的 objectfile（编译前必须确定）
    after_load(function (target)
        local autogenDir = path.join(os.projectdir(), "Src/Proto/AutoGen")
        local sourcebatch = target:sourcebatches()["proto_gen"]
        if not sourcebatch then
            return
        end
        for _, sourcefile_proto in ipairs(sourcebatch.sourcefiles) do
            local pbcc = path.join(autogenDir, path.basename(sourcefile_proto) .. ".pb.cc")
            local objfile = target:objectfile(pbcc)
            table.insert(target:objectfiles(), objfile)
        end
    end)

    -- 阶段 2: protoc 生成 .pb.{h,cc}
    -- on_preparecmd_file(function (target, batchcmds, sourcefile_proto, opt)
    --     local protocTarget = target:dep("protoc")
    --     local protoc = path.join(protocTarget:targetdir(), protocTarget:name())

    --     local protoDir   = path.join(os.projectdir(), "Src/Proto")
    --     local autogenDir = path.join(os.projectdir(), "Src/Proto/AutoGen")
    --     local protoFile  = path.filename(sourcefile_proto)

    --     batchcmds:mkdir(autogenDir)

    --     batchcmds:show_progress(opt.progress, "${color.build.object}proto.gen %s", sourcefile_proto)
    --     batchcmds:vrunv(protoc, {
    --         "--proto_path=" .. protoDir,
    --         "--cpp_out=" .. autogenDir,
    --         protoFile
    --     }, {curdir = protoDir})

    --     -- 增量依赖
    --     batchcmds:add_depfiles(sourcefile_proto)
    --     batchcmds:set_depcache(target:dependfile(
    --         path.join(autogenDir, path.basename(sourcefile_proto) .. ".pb.cc")))
    -- end)

    -- 阶段 3: 编译 .pb.cc
    on_buildcmd_file(function (target, batchcmds, sourcefile_proto, opt)
        local protocTarget = target:dep("protoc")
        local protoc = path.join(protocTarget:targetdir(), protocTarget:name())

        local protoDir   = path.join(os.projectdir(), "Src/Proto")
        local autogenDir = path.join(os.projectdir(), "Src/Proto/AutoGen")
        local protoFile  = path.filename(sourcefile_proto)

        batchcmds:show_progress(opt.progress, "${color.build.object}proto.gen %s", sourcefile_proto)
        batchcmds:vrunv(protoc, {
            "--proto_path=" .. protoDir,
            "--cpp_out=" .. autogenDir,
            protoFile
        }, {curdir = protoDir})

        -- 增量依赖
        batchcmds:add_depfiles(sourcefile_proto)
        batchcmds:set_depcache(target:dependfile(
            path.join(autogenDir, path.basename(sourcefile_proto) .. ".pb.cc")))

        local autogenDir = path.join(os.projectdir(), "Src/Proto/AutoGen")
        local pbcc  = path.join(autogenDir, path.basename(sourcefile_proto) .. ".pb.cc")
        local objfile = target:objectfile(pbcc)

        batchcmds:show_progress(opt.progress, "${color.build.object}compiling.proto.$(mode) %s", pbcc)
        batchcmds:compile(pbcc, objfile, {configs = {includedirs = autogenDir}})
        batchcmds:set_depmtime(os.mtime(objfile))
        batchcmds:set_depcache(target:dependfile(objfile))
    end)

target("Proto")
    set_kind("object")
    set_warnings("none")
    add_rules("proto_msgid", "proto_gen")
    add_deps("protobuf", "protoc")
    -- set_policy("build.across_targets_in_parallel", false)
    add_files("*.proto")
    add_includedirs("$(projectdir)/Src/Proto/AutoGen", {public = true})
