--- @file xmake.lua
--- @brief Massive MMO Server — 顶层构建配置
--- 聚合 Src/ 所有子 target

set_project("Massive")
set_version("0.1.0")

set_languages("c++23")
add_rules("mode.debug", "mode.release")

--- MSVC: /utf-8 是强制要求 (fmt base.h 会 static_assert 检查)
if is_plat("windows") then
    add_cxflags("/utf-8")
end

--- 全局警告策略: 项目代码全开 Warning
set_warnings("all")

add_rules("plugin.compile_commands.autoupdate", {outputdir = "$(projectdir)/build", lsp='clangd'})

set_targetdir(path.join("Bin", "$(plat)-$(arch)-$(mode)"))

set_rundir(os.projectdir())

--- 全局编译器标志
if is_mode("debug") then
    set_optimize("none")
    add_defines("MASSIVE_ENABLE_TRACY")
    set_symbols("debug")
end

if is_mode("release") then
    set_optimize("fastest")
    set_symbols("hidden")
end

--- 项目源码全局头文件路径
add_includedirs("Src")

--- 第三方依赖全部由 ThirdParty/xmake.lua 管理
includes("ThirdParty/xmake.lua")

--- 聚合 Common 子 target
includes("Src/Common/xmake.lua")

--- 聚合各服务器进程
includes("Src/Login/xmake.lua")
includes("Src/Gate/xmake.lua")
includes("Src/World/xmake.lua")
includes("Src/Center/xmake.lua")
includes("Src/Social/xmake.lua")

--- 聚合 Proto
includes("Src/Proto/xmake.lua")

--
-- 自定义 format task：格式化项目源码，排除 ThirdParty 和 AutoGen 目录
--
task("format")
    set_category("plugin")
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        import("core.project.project")
        import("lib.detect.find_tool")
        import("async.runjobs")
        import("utils.progress")
        import("private.action.require.impl.packagenv")
        import("private.action.require.impl.install_packages")

        -- 加载当前项目的配置
        config.load()

        -- 查找 clang-format
        local oldenvs = packagenv.enter("llvm")
        local packages = {}
        local clang_format = find_tool("clang-format")
        if not clang_format then
            table.join2(packages, install_packages("llvm"))
        end
        for _, instance in ipairs(packages) do
            instance:envs_enter()
        end
        if not clang_format then
            clang_format = find_tool("clang-format", {force = true})
        end
        assert(clang_format, "clang-format not found!")

        -- 构造 clang-format 参数
        local argv = {}
        local projectdir = os.projectdir()

        -- 尝试使用项目根目录的 .clang-format
        local stylefile = path.join(projectdir, ".clang-format")
        if os.isfile(stylefile) then
            table.insert(argv, "--style=file:" .. stylefile)
        elseif option.get("style") then
            table.insert(argv, "--style=" .. option.get("style"))
        end

        if option.get("dry-run") then
            table.insert(argv, "--dry-run")
        else
            table.insert(argv, "-i")
        end

        -- 收集 Src/ 下所有 C++ 文件，排除 AutoGen 目录
        local sourcefiles = {}
        local patterns = {"Src/**.cpp", "Src/**.cc", "Src/**.cxx", "Src/**.h", "Src/**.hpp"}

        -- 排除规则
        local function is_excluded(filepath)
            -- 排除路径中包含 AutoGen 的文件（代码生成文件）
            if filepath:find("[\\/]AutoGen[\\/]") then
                return true
            end
            return false
        end

        -- 收集源文件，并转为绝对路径
        for _, pat in ipairs(patterns) do
            local absPattern = path.join(projectdir, pat)
            for _, f in ipairs(os.files(absPattern)) do
                if not is_excluded(f) then
                    table.insert(sourcefiles, f)
                end
            end
        end

        -- 去重
        local seen = {}
        local uniqueFiles = {}
        for _, f in ipairs(sourcefiles) do
            if not seen[f] then
                seen[f] = true
                table.insert(uniqueFiles, f)
            end
        end
        sourcefiles = uniqueFiles

        -- 并行格式化
        if #sourcefiles > 0 then
            local jobs = tonumber(option.get("jobs"))
            if not jobs or jobs <= 0 then
                jobs = os.default_njob()
            end
            local format_time = os.mclock()
            local runjobs_opt = {
                total = #sourcefiles,
                comax = jobs,
                showtips = false,
                progress_refresh = true
            }
            runjobs("clang-format", function (index, total, opt)
                local sourcefile = sourcefiles[index]
                local format_argv = table.join(argv, {sourcefile})
                progress.show(opt.progress, "formatting %s", path.relative(sourcefile, projectdir))
                os.execv(clang_format.program, format_argv, {curdir = projectdir})
            end, runjobs_opt)
            format_time = os.mclock() - format_time
            progress.show(100, "${color.success}formatted %d files, spent %.3fs", #sourcefiles, format_time / 1000)
        else
            cprint("${color.warning}no source files found in Src/")
        end
        os.setenvs(oldenvs)
    end)

    set_menu {
        usage = "xmake format [options]",
        description = "Format project source files under Src/, excluding ThirdParty and AutoGen.",
        options = {
            {'n', "dry-run", "k", nil, "Do not make any changes, just show the files that would be formatted."},
            {'j', "jobs",    "kv", tostring(os.default_njob()),
                                       "Set the number of parallel format jobs."},
            {'s', "style",   "kv", nil, "Set the path of .clang-format file or a coding style.",
                                       values = {"LLVM", "Google", "Chromium", "Mozilla", "WebKit"}}
        }
    }
