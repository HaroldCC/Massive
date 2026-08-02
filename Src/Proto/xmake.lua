--- @file xmake.lua
--- @brief Proto — 协议库（GenMsgID + protoc 代码生成）
---
---
--- 构建时序 (build.fence + add_deps):
---   1. protoc target 全量编译（含链接 protoc.exe）
---   2. Proto build 阶段开始
---      a. on_buildcmd_file: 运行 protoc 生成 .pb.{h,cc} → 编译 .pb.cc
---
--- 客户端协议（*.proto）+ 内部 RPC 协议（Internal/*.proto）共用规则。
--- protoc 输出结构：
---   Common.proto → AutoGen/Common.pb.cc
---   Internal/CenterRPC.proto → AutoGen/Internal/CenterRPC.pb.cc
--- (protoc 自动在 --cpp_out 中保留 --proto_path 剥离后的子目录)

rule("proto_msgid")
    on_config(function (target)
        local protoDir   = path.join(os.projectdir(), "Src/Proto")
        local genScript  = path.join(os.projectdir(), "Tools/Proto/GenMsgID.py")

        -- 客户端 MsgID.proto
        local msgidProto = path.join(protoDir, "MsgID.proto")
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
            os.vrunv("python", {genScript, "--proto-dir", protoDir, "--enum-name", "EMsgID",
                                "--output", "MsgID.proto"})
            cprint("${color.success}[proto] MsgID.proto 已更新")
        end

        -- 内部 InternalMsgID.proto
        local internalProto = path.join(protoDir, "Internal", "InternalMsgID.proto")
        local internalFiles = os.files(path.join(protoDir, "Internal", "*.proto"))
        local internalDirty = not os.isfile(internalProto)
        if not internalDirty then
            local internalMtime = os.mtime(internalProto)
            for _, f in ipairs(internalFiles) do
                if path.filename(f) ~= "InternalMsgID.proto" and os.mtime(f) > internalMtime then
                    internalDirty = true
                    break
                end
            end
        end
        if internalDirty then
            os.vrunv("python", {genScript, "--proto-dir", protoDir,
                                "--subdir", "Internal",
                                "--enum-name", "EInternalMsgID",
                                "--output", "Internal/InternalMsgID.proto"})
            cprint("${color.success}[proto] InternalMsgID.proto 已更新")
        end

        -- EMsgID 枚举绑定（公共层，DasCommonModule 依赖）——MsgID.proto 就绪后立即生成，
        -- 保证 ScriptEngine 编译时文件已存在（ScriptEngine 的 gen_emsgid_bind 只负责 add_files）
        local bindScript   = path.join(os.projectdir(), "Tools/Script/GenMsgBindings.py")
        local emsgidCpp    = path.join(protoDir, "AutoGen", "EMsgIDBind.gen.cpp")
        local emsgidDirty  = not os.isfile(emsgidCpp)
        if not emsgidDirty then
            local bindMtime = os.mtime(emsgidCpp)
            if os.mtime(msgidProto) > bindMtime then
                emsgidDirty = true
            end
        end
        if emsgidDirty then
            os.vrunv("python", {bindScript, "--only-emsgid", "--service", "world",
                                "--proto-dir", protoDir,
                                "--cpp-out", path.join(protoDir, "AutoGen"),
                                "--emsgid-out", path.join(protoDir, "AutoGen")})
            cprint("${color.success}[proto] EMsgIDBind.gen 已更新")
        end
    end)

--- rule: protoc 生成 .pb.{h,cc} + 编译
rule("proto_gen")
    set_extensions(".proto")

    add_deps("c++", "protoc")

    -- 阶段 1: 预注册 .pb.cc 的 objectfile（编译前必须确定）
    after_load(function (target)
        local autogenDir = path.join(os.projectdir(), "Src/Proto/AutoGen")
        local sourcebatch = target:sourcebatches()["proto_gen"]
        if not sourcebatch then
            return
        end
        -- protoc 输出到 AutoGen/ 下保留子目录结构，所以 pbcc 用相对路径
        -- path.basename(protoRel) 会丢掉子目录，直接让 protoc 决定输出路径
        local protoDir = target:scriptdir()
        for _, sourcefile_proto in ipairs(sourcebatch.sourcefiles) do
            local protoRel = path.relative(sourcefile_proto, protoDir):gsub("\\", "/")
            -- 去掉 .proto 后缀，保留子目录：Internal/CenterRPC.proto → Internal/CenterRPC.pb.cc
            local noExt = protoRel:gsub("%.proto$", "")
            local pbcc = path.join(autogenDir, noExt .. ".pb.cc")
            local objfile = target:objectfile(pbcc)
            table.insert(target:objectfiles(), objfile)
        end
    end)

    -- 阶段 2: protoc 生成 .pb.{h,cc} + 编译
    on_buildcmd_file(function (target, batchcmds, sourcefile_proto, opt)
        local protocTarget = target:dep("protoc")
        local protoc = path.join(protocTarget:targetdir(), protocTarget:name())

        assert(protoc, "protoc not found!")

        local protoDir   = path.join(os.projectdir(), "Src/Proto")
        local autogenDir = path.join(os.projectdir(), "Src/Proto/AutoGen")

        -- 传相对路径（如 Internal/CenterRPC.proto），
        -- protoc 剥离 --proto_path 前缀后输出到 AutoGen/Internal/CenterRPC.pb.cc
        local protoRel = path.relative(sourcefile_proto, protoDir):gsub("\\", "/")
        local pbccBase = protoRel:gsub("%.proto$", "")
        local pbcc = path.join(autogenDir, pbccBase .. ".pb.cc")

        batchcmds:show_progress(opt.progress, "${color.build.object}proto.gen %s", protoRel)
        batchcmds:mkdir(autogenDir)
        -- -- 声明 protoc 本身为依赖文件，若不存在则触发重新链接
        -- batchcmds:add_depfiles(protoc)
        batchcmds:vrunv(protoc, {
            "--proto_path=" .. protoDir,
            "--cpp_out=" .. autogenDir,
            protoRel
        }, {curdir = protoDir})

        -- 增量依赖（.pb.cc 生成步骤）
        batchcmds:add_depfiles(sourcefile_proto)
        -- 用 set_depmtime 记录 .pb.cc 的 mtime，文件被删时 depcache 会失效
        -- batchcmds:set_depmtime(os.mtime(pbcc))
        -- batchcmds:set_depcache(target:dependfile(pbcc))

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
    add_files("*.proto")
    add_files("Internal/**.proto")
    add_includedirs("$(projectdir)/Src/Proto/AutoGen", {public = true})
    set_policy("build.fence", true)
