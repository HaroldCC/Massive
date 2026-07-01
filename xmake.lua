--- @file xmake.lua
--- @brief Massive MMO Server — 顶层构建配置
--- 聚合 Src/ 所有子 target

set_project("Massive")
set_version("0.1.0")

set_languages("c++23")
add_rules("mode.debug", "mode.release", "mode.releasedbg")

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
    add_defines("MASSIVE_ENABLE_TRACY")
elseif is_mode("releasedbg") then
    add_defines("MASSIVE_ENABLE_TRACY")
    if is_plat("linux") then
        add_ldflags("-rdynamic")
    elseif is_plat("windows") then
        add_ldflags("/DEBUG:FULL")
    end
elseif is_mode("release") then
end

add_includedirs("Src")

includes("ThirdParty/xmake.lua")
includes("Src/Common/xmake.lua")
includes("Src/Login/xmake.lua")
includes("Src/Gate/xmake.lua")
includes("Src/World/xmake.lua")
includes("Src/Center/xmake.lua")
includes("Src/Social/xmake.lua")
includes("Src/TestClient/xmake.lua")
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

        config.load()

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

        local argv = {}
        local projectdir = os.projectdir()

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

        local sourcefiles = {}
        local patterns = {"Src/**.cpp", "Src/**.cc", "Src/**.cxx", "Src/**.h", "Src/**.hpp"}

        local function is_excluded(filepath)
            if filepath:find("[\\/]AutoGen[\\/]") then
                return true
            end
            return false
        end

        for _, pat in ipairs(patterns) do
            local absPattern = path.join(projectdir, pat)
            for _, f in ipairs(os.files(absPattern)) do
                if not is_excluded(f) then
                    table.insert(sourcefiles, f)
                end
            end
        end

        local seen = {}
        local uniqueFiles = {}
        for _, f in ipairs(sourcefiles) do
            if not seen[f] then
                seen[f] = true
                table.insert(uniqueFiles, f)
            end
        end
        sourcefiles = uniqueFiles

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

--
-- 自定义 up/down/logs task：本地开发环境一键起服/停服/看日志
-- xmake run <target> 通过 set_targetdir 路径找二进制并阻塞执行（Ctrl+C 退出）。
-- 这里用 cmd /c start /B (Win) / & (Unix) 做后台启动 + 端口轮询做健康检查。
--
local _servers = {
    { name = "CenterServer", bin = "CenterServer.exe", port = 9660, delay_ms = 1000 },
    { name = "WorldServer",  bin = "WorldServer.exe",  port = 9770, delay_ms = 2000 },
    { name = "LoginServer",  bin = "LoginServer.exe",  port = 9551, delay_ms = 1000 },
    { name = "GateServer",   bin = "GateServer.exe",   port = 9550, delay_ms = 2000 },
}

local function _is_win() return os.host() == "windows" end

local function _pids_dir()
    return path.join(os.projectdir(), ".pids")
end

task("up")
    set_category("run")
    on_run(function ()
        import("core.project.config")
        config.load()
        local mode = config.get("mode") or "debug"
        local bdir = path.join(os.projectdir(), "Bin", os.host() .. "-" .. os.arch() .. "-" .. mode)
        local projectdir = os.projectdir()
        os.mkdir(_pids_dir())

        cprint("${bright green}═════════════════════════════════")
        cprint("   Massive — 一键起服")
        cprint("═════════════════════════════════${clear}")

        for _, srv in ipairs(_servers) do
            local bin = path.join(bdir, srv.bin)
            if not os.isfile(bin) then
                cprint("${red}✗ %s: 找不到二进制 → %s${clear}", srv.name, bin)
                cprint("${yellow}  请先执行 xmake build${clear}")
                return
            end
            cprint("${yellow}▶${clear} %-12s starting...", srv.name)
            if _is_win() then
                -- start /B 后台 + 日志重定向到 logs/ 避免终端泄漏
                os.mkdir(path.join(bdir, "logs"))
                local logfile = path.join(bdir, "logs", srv.name:lower() .. ".log")
                os.exec("cmd /c start /B \"\" \"" .. bin .. "\" > \"" .. logfile .. "\" 2>&1", {curdir = projectdir})
            else
                os.exec("\"" .. bin .. "\" >/dev/null 2>&1 & echo $! > "
                    .. _pids_dir() .. "/" .. srv.name:lower() .. ".pid", {curdir = projectdir})
            end
            os.sleep(srv.delay_ms)
        end

        os.sleep(1000)
        cprint("${bright green}──── 端口健康检查 ────${clear}")
        local all_ok = true
        for _, srv in ipairs(_servers) do
            local ok = false
            if _is_win() then
                ok = os.exec("cmd /c \"netstat -ano | findstr \\\"LISTENING\\\" | findstr \\\":" .. srv.port .. "\\\" >nul 2>nul\"")
            else
                ok = os.exec("ss -tlnp 2>/dev/null | grep " .. srv.port
                    .. " || netstat -tlnp 2>/dev/null | grep " .. srv.port)
            end
            if ok then
                cprint("${green}✓ %-12s :%d (LISTEN)${clear}", srv.name, srv.port)
            else
                cprint("${red}✗ %-12s :%d (NOT LISTENING)${clear}", srv.name, srv.port)
                all_ok = false
            end
        end

        if all_ok then
            cprint("${bright green}═════════════════════════════════")
            cprint("   全部就绪 ✓")
            cprint("   xmake logs   — 查看日志")
            cprint("   xmake down   — 停服")
            cprint("═════════════════════════════════${clear}")
        else
            cprint("${yellow}部分服务未就绪，请检查日志: xmake logs${clear}")
        end
    end)

    set_menu {
        usage = "xmake up",
        description = "一键起服：Center → World → Login → Gate + 端口健康检查。"
    }

task("down")
    set_category("run")
    on_run(function ()
        cprint("${bright yellow}═════════════════════════════════")
        cprint("   Massive — 停服")
        cprint("═════════════════════════════════${clear}")

        for i = #_servers, 1, -1 do
            local srv = _servers[i]
            local ok = os.execv("taskkill", {"/F", "/IM", srv.bin}, {curdir = os.projectdir()})
            if ok then
                cprint("${yellow}✗${clear} %s", srv.name)
            else
                cprint("${yellow}?${clear} %s (未启动)", srv.name)
            end
            os.sleep(300)
        end

        os.tryrm(_pids_dir())
        cprint("${green}全部服务已停止.${clear}")
    end)

    set_menu {
        usage = "xmake down",
        description = "停服：Gate → Login → World → Center。"
    }

task("logs")
    set_category("run")
    on_run(function ()
        import("core.project.config")
        config.load()
        local mode = config.get("mode") or "debug"
        local logdir = path.join(os.projectdir(), "Bin", os.host() .. "-" .. os.arch() .. "-" .. mode, "logs")
        if not os.isdir(logdir) then
            cprint("${red}日志目录不存在: %s${clear}", logdir)
            cprint("${yellow}请先执行 xmake up 启动服务${clear}")
            return
        end

        local logfiles = {}
        for _, srv in ipairs(_servers) do
            local log = path.join(logdir, srv.name:lower() .. ".log")
            if os.isfile(log) then
                table.insert(logfiles, "\"" .. log .. "\"")
            end
        end

        if #logfiles == 0 then
            cprint("${red}没有找到日志文件${clear}")
            return
        end

        cprint("${cyan}=== Tailing %d logs (Ctrl+C 退出) ===${clear}", #logfiles)
        if _is_win() then
            os.exec("powershell -Command \"Get-Content -Wait -Tail 20 "
                .. table.concat(logfiles, ",") .. "\"", {curdir = os.projectdir()})
        else
            os.exec("tail -f " .. table.concat(logfiles, " "), {curdir = os.projectdir()})
        end
    end)

    set_menu {
        usage = "xmake logs",
        description = "tail -f 模式查看所有服务日志。"
    }
