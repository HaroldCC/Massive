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
--
-- 设计原则：
--   xmake.lua 保有服务器拓扑表 _servers（唯一数据源），通过 stdin JSON 传给 Python。
--   Python (Tools/ServerCtl.py) 负责所有进程管理、端口探测、日志跟踪等具体操作。
--   xmake task 壳只做：找 Python + 拼 JSON payload + os.execv。
--
-- 注意：import/find_tool/config.load 等沙箱函数只在 on_run 内可用，
--       所以辅助函数也定义在各 on_run 内部。文件级只放纯数据 + 纯字符串函数。
--
local _servers = {
    { name = "CenterServer", bin = "CenterServer.exe", port = 9660, start_order = 1,
      config = "Config/center.toml" },
    { name = "WorldServer",  bin = "WorldServer.exe",  port = 9770, start_order = 2,
      config = "Config/world.toml",  key_config = "Config/login.key" },
    { name = "LoginServer",  bin = "LoginServer.exe",  port = 9551, start_order = 3,
      config = "Config/login.toml",  key_config = "Config/login.key" },
    { name = "GateServer",   bin = "GateServer.exe",   port = 9550, start_order = 4,
      config = "Config/gate.toml" },
}
table.sort(_servers, function(a, b) return a.start_order < b.start_order end)

--- 文件级纯函数：计算二进制目录
--- @param  mode  string  "debug"|"release"|"releasedbg"
--- @return       string  absolute path
local function _bindir(mode)
    return path.join(os.projectdir(), "Bin", os.host() .. "-" .. os.arch() .. "-" .. (mode or "debug"))
end


-- =========================================================================
-- task: up
-- =========================================================================
task("up")
    set_category("run")
    on_run(function ()
        import("core.base.json")
        import("core.project.config")
        config.load()
        local mode = config.get("mode") or "debug"
        local payload = json.encode({
            action = "up",
            project_dir = os.projectdir(),
            bin_dir = _bindir(mode),
            pids_dir = path.join(os.projectdir(), ".pids"),
            servers = _servers,
        })
        local payload_file = path.join(os.projectdir(), ".pids", "payload.json")
        os.mkdir(path.join(os.projectdir(), ".pids"))
        io.writefile(payload_file, payload)
        -- cmd /c 强制走 Windows cmd.com / cmd.exe，绕过 Scoop shim 的 CreateProcess 问题
        os.exec("cmd /c python Tools/ServerCtl.py \"" .. payload_file .. "\"",
            {curdir = os.projectdir()})
    end)
    set_menu {
        usage = "xmake up",
        description = "一键起服：Center → World → Login → Gate + 端口健康检查。"
    }


-- =========================================================================
-- task: down
-- =========================================================================
task("down")
    set_category("run")
    on_run(function ()
        import("core.base.json")
        import("core.project.config")
        config.load()
        local mode = config.get("mode") or "debug"
        local payload = json.encode({
            action = "down",
            project_dir = os.projectdir(),
            bin_dir = _bindir(mode),
            pids_dir = path.join(os.projectdir(), ".pids"),
            servers = _servers,
        })
        local payload_file = path.join(os.projectdir(), ".pids", "payload.json")
        os.mkdir(path.join(os.projectdir(), ".pids"))
        io.writefile(payload_file, payload)
        os.exec("cmd /c python Tools/ServerCtl.py \"" .. payload_file .. "\"",
            {curdir = os.projectdir()})
    end)
    set_menu {
        usage = "xmake down",
        description = "停服：Gate → Login → World → Center（读 pidfile 精准终止）。"
    }


-- =========================================================================
-- task: logs
-- =========================================================================
task("logs")
    set_category("run")
    on_run(function ()
        import("core.base.json")
        import("core.project.config")
        config.load()
        local mode = config.get("mode") or "debug"
        local payload = json.encode({
            action = "logs",
            project_dir = os.projectdir(),
            bin_dir = _bindir(mode),
            pids_dir = path.join(os.projectdir(), ".pids"),
            servers = _servers,
        })
        local payload_file = path.join(os.projectdir(), ".pids", "payload.json")
        os.mkdir(path.join(os.projectdir(), ".pids"))
        io.writefile(payload_file, payload)
        os.exec("cmd /c python Tools/ServerCtl.py \"" .. payload_file .. "\"",
            {curdir = os.projectdir()})
    end)
    set_menu {
        usage = "xmake logs",
        description = "跟踪所有服务日志（Win：独立窗口 / Linux：tail -f）。"
    }


-- =========================================================================
-- task: testclient — 多客户端压测工具入口
--
-- 用法:
--   xmake testclient                         # 默认配置
--   xmake testclient -- --config Config/xxx.toml          # 指定配置
--   xmake testclient -- --count 50 --duration 120           # 覆盖参数
--   xmake testclient -- --verbose                           # verbose 模式
--
-- 注意：-- 后的参数原样透传给 TestClient 二进制。
-- =========================================================================
task("testclient")
    set_category("run")
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        import("core.project.project")
        config.load()
        local mode = config.get("mode") or "debug"

        -- 确保 target 存在
        local target = project.target("TestClient")
        if not target then
            cprint("${color.error}testclient: target TestClient not found")
            return
        end

        -- 检查二进制，兜底自动编译
        local bin = path.join(_bindir(mode), "TestClient.exe")
        if not os.isfile(bin) then
            cprint("${color.warning}testclient: building TestClient...")
            local ok = os.exec("xmake build TestClient", {curdir = os.projectdir()})
            if ok ~= 0 then
                cprint("${color.error}testclient: build failed!")
                return
            end
        end

        -- 取透传参数（xmake 的 "--" 分隔符后的全部参数）
        local raw = option.get("arguments") or ""
        if raw == "" then
            raw = "--config Config/testclient.toml"
        end

        cprint("${color.success}testclient: running...")
        -- cmd /c 强制走 Windows cmd.exe，绕过 Scoop shim CreateProcess 问题
        os.exec("cmd /c \"" .. bin .. "\" " .. raw, {curdir = os.projectdir()})
    end)
    set_menu {
        usage = "xmake testclient [-- args...]",
        description = "启动多客户端压测工具 TestClient。\n" ..
                      "在 -- 后传递所有 TestClient 参数。无参数时默认用 Config/testclient.toml。\n" ..
                      "  xmake testclient                                     # 默认配置\n" ..
                      "  xmake testclient -- --config Config/my.toml          # 指定配置\n" ..
                      "  xmake testclient -- --count 100 --duration 300       # 覆盖参数"
    }
