-- @note EMsgID 枚举绑定（EMsgIDBind.gen.{h,cpp}）由生成器产出到公共层 Src/Proto/AutoGen。
--       生成主要由 Proto target 的 proto_msgid rule 在 MsgID.proto 就绪后触发；
--       本 rule 兜底：若文件缺失（干净 checkout 极端时序）则补一次 --only-emsgid 生成，
--       并负责 add_files 纳入 ScriptEngine 编译。不在本 target 硬编码生成产物路径。

rule("gen_emsgid_bind")
    on_config(function (target)
        local protoDir   = path.join(os.projectdir(), "Src/Proto")
        local genScript  = path.join(os.projectdir(), "Tools/Script/GenMsgBindings.py")
        local emsgidCpp  = path.join(protoDir, "AutoGen", "EMsgIDBind.gen.cpp")
        local msgRegCpp  = path.join(protoDir, "AutoGen", "MsgTypeRegistry.gen.cpp")

        -- 兜底：文件缺失才补生成（正常路径由 Proto 的 proto_msgid rule 已生成）
        if not os.isfile(emsgidCpp) or not os.isfile(msgRegCpp) then
            os.vrunv("python", {genScript, "--only-emsgid", "--service", "world",
                                "--proto-dir", protoDir,
                                "--cpp-out", path.join(protoDir, "AutoGen"),
                                "--emsgid-out", path.join(protoDir, "AutoGen")})
            cprint("${color.success}[emsgid] EMsgIDBind.gen 兜底生成")
        end

        -- 纳入编译（always_added 兜底：若上述生成因 MsgID.proto 未就绪而跳过，编译期也不报文件缺失）
        target:add("files", emsgidCpp, {always_added = true})
        -- 消息宏命名查表（ScriptLayer_06 §3.1）——与 EMsgIDBind 同源，公共层
        target:add("files", msgRegCpp, {always_added = true})
    end)

target("ScriptEngine")
    set_kind("static")
    add_headerfiles("**.h")
    add_files("**.cpp")
    add_includedirs("$(projectdir)/Src/Proto/AutoGen")
    add_rules("gen_emsgid_bind")
    add_deps("CommonCore", "CommonLog", "CommonTimer", "CommonCrypto", "libDaScript", "Proto", {public = true})