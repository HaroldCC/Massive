--- @file xmake.lua
--- @brief ThirdParty — 第三方依赖统一声明
---
--- A 组: 外部编译产物 (OpenSSL, Protobuf, daScript, libpq)
---   → headeronly 封装 Bin/ 路径
--- B 组: xmake 内编译为静态库 (Fmt, TracyClient)
--- C 组: 纯头文件 (Asio, EnTT, Spdlog, ConcurrentQueue)
---
--- 所有 include/linkdir 路径指向 ThirdParty/Bin/<name>/{include,lib}。
--- $(projectdir) 是 xmake 内置变量，展开为项目根目录。

--- @section A 组: 外部编译产物（headeronly 封装路径信息）

target("OpenSSL")
    set_kind("headeronly")
    if is_plat("windows") then
        --- Windows: vendored 二进制
        add_includedirs("$(projectdir)/ThirdParty/Bin/openssl/include", {public = true})
        add_linkdirs("$(projectdir)/ThirdParty/Bin/openssl/Win64", {public = true})
        add_links("libcrypto", "libssl", {public = true})
    elseif is_plat("linux") then
        --- Linux: 系统 libssl-dev (apt install libssl-dev)
        add_links("ssl", "crypto", {public = true})
    end

target("Protobuf")
    set_kind("headeronly")
    add_includedirs("$(projectdir)/ThirdParty/Bin/protobuf/include", {public = true})
    add_linkdirs("$(projectdir)/ThirdParty/Bin/protobuf/lib", {public = true})
    --- protobuf 依赖 libprotobuf + 全部 absl 静态库，扫描 lib 目录自动链接
    on_load(function (target)
        import("core.project.project")
        local libdir = path.join(os.projectdir(), "ThirdParty/Bin/protobuf/lib")
        for _, libpath in ipairs(os.files(path.join(libdir, "*.lib"))) do
            local name = path.basename(libpath)
            target:add("links", name, {public = true})
        end
    end)

target("DaScript")
    set_kind("headeronly")
    add_includedirs("$(projectdir)/ThirdParty/Bin/dasScript/include", {public = true})
    add_linkdirs("$(projectdir)/ThirdParty/Bin/dasScript/lib", {public = true})

target("LibPQ")
    set_kind("headeronly")
    if is_plat("windows") then
        --- Windows: vendored 二进制
        add_includedirs("$(projectdir)/ThirdParty/Bin/libpq/include", {public = true})
        add_linkdirs("$(projectdir)/ThirdParty/Bin/libpq/Win64", {public = true})
        add_links("libpq", {public = true})
    elseif is_plat("linux") then
        --- Linux: 系统 libpq-dev (apt install libpq-dev)
        add_sysincludedirs("/usr/include/postgresql", {public = true})
        add_links("pq", {public = true})
    end

--- @section B 组: xmake 内编译为静态库

target("Fmt")
    set_kind("static")
    set_warnings("none")
    add_files("fmt/src/format.cc")
    add_includedirs("$(projectdir)/ThirdParty/fmt/include", {public = true})

target("TracyClient")
    set_kind("static")
    set_warnings("none")
    add_files("tracy/public/TracyClient.cpp")
    add_includedirs("$(projectdir)/ThirdParty/tracy/public", {public = true})
    add_defines("TRACY_ENABLE")

--- @section C 组: 纯头文件

target("Asio")
    set_kind("headeronly")
    add_includedirs("$(projectdir)/ThirdParty/asio/asio/include", {public = true})

target("EnTT")
    set_kind("headeronly")
    add_includedirs("$(projectdir)/ThirdParty/entt/src", {public = true})

target("Spdlog")
    set_kind("headeronly")
    add_includedirs("$(projectdir)/ThirdParty/spdlog/include", {public = true})
    add_deps("Fmt")         --- spdlog 需要 fmt 头文件

target("ConcurrentQueue")
    set_kind("headeronly")
    add_includedirs("$(projectdir)/ThirdParty/concurrentqueue", {public = true})
