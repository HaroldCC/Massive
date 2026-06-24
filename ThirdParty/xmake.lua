--- @file xmake.lua
--- @brief ThirdParty — 第三方依赖统一声明
---
--- 所有依赖通过独立的 .lua 文件管理，本文件统一 include。
--- 命名风格与 CMake 一致（小写），避免与 xmake-repo 冲突。
---
--- 编译型依赖：
---   protobuf.lua → abseil / utf8_range / libprotobuf / protoc
---   fmt.lua      → fmt
---   tracy.lua    → tracy
---   daslang.lua  → dascript
---
--- Header-only/预编译 在此定义：
---   asio, entt, spdlog, concurrentqueue, tomlplusplus
---   openssl, libpq
---
--- 所有 ThirdParty target 使用 rule("thirdparty")，自动：
---   - 分到 "ThirdParty" group
---   - 默认不构建（set_default(false)），被 add_deps 引用时才编译
---   - 用 add_sysincludedirs 避免第三方警告泄漏

-- =============================================================================
-- rule: thirdparty — 统一第三方库设置
-- =============================================================================

rule("Rules.ThirdParty")
    on_config(function (target)
        target:set("group", "ThirdParty")
        target:set("default", false)
        target:set("targetdir", path.join(target:targetdir(), "ThirdParty"))
    end)

-- =============================================================================
-- 编译型依赖（独立 .lua 文件）
-- =============================================================================

includes("protobuf.lua")
includes("fmt.lua")
includes("tracy.lua")
includes("daslang.lua")

-- =============================================================================
-- openssl: vendored 二进制
-- =============================================================================

target("openssl")
    set_kind("headeronly")
    add_rules("Rules.ThirdParty")
    if is_plat("windows") then
        add_sysincludedirs("$(projectdir)/ThirdParty/Bin/openssl/include", {public = true})
        add_linkdirs("$(projectdir)/ThirdParty/Bin/openssl/Win64", {public = true})
        add_links("libcrypto", "libssl", {public = true})
        add_syslinks("user32", "crypt32", {public = true})
    elseif is_plat("linux") then
        add_links("ssl", "crypto", {public = true})
    end

-- =============================================================================
-- libpq: vendored 二进制
-- =============================================================================

target("libpq")
    set_kind("headeronly")
    add_rules("Rules.ThirdParty")
    if is_plat("windows") then
        add_sysincludedirs("$(projectdir)/ThirdParty/Bin/libpq/include", {public = true})
        add_linkdirs("$(projectdir)/ThirdParty/Bin/libpq/Win", {public = true})
        add_links("libpq", {public = true})
    elseif is_plat("linux") then
        add_sysincludedirs("$(projectdir)/ThirdParty/Bin/libpq/Linux", {public = true})
        add_links("pq", {public = true})
    end

-- =============================================================================
-- Header-only 库
-- =============================================================================

target("asio")
    set_kind("headeronly")
    add_rules("Rules.ThirdParty")
    add_sysincludedirs(path.join(os.projectdir(), "ThirdParty/asio/asio/include"), {public = true})

target("entt")
    set_kind("headeronly")
    add_rules("Rules.ThirdParty")
    add_sysincludedirs(path.join(os.projectdir(), "ThirdParty/entt/src"), {public = true})

target("spdlog")
    set_kind("headeronly")
    add_rules("Rules.ThirdParty")
    add_sysincludedirs(path.join(os.projectdir(), "ThirdParty/spdlog/include"), {public = true})
    add_deps("fmt")

target("concurrentqueue")
    set_kind("headeronly")
    add_rules("Rules.ThirdParty")
    add_sysincludedirs(path.join(os.projectdir(), "ThirdParty/concurrentqueue"), {public = true})

target("tomlplusplus")
    set_kind("headeronly")
    add_rules("Rules.ThirdParty")
    add_sysincludedirs(path.join(os.projectdir(), "ThirdParty/tomlplusplus/include"), {public = true})
