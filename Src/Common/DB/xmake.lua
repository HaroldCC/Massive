--- @file xmake.lua
--- @brief CommonDB — 数据库层（DBWorkerPool + DBRange + Column）
---
--- 构建流程（增量）:
---   1. db_gen rule: DDL 文件变化 → GenDBBindings.py 增量生成 AutoGen/*.gen.h
---   2. 编译 *.gen.h（headeronly，无需额外编译）

--- rule: 扫描 DDL，增量生成 .gen.h
rule("db_gen")
    on_config(function (target)
        local genScript = path.join(os.projectdir(), "Tools/DB/GenDBBindings.py")
        local sqlDir    = path.join(os.projectdir(), "Tools/DB/SQL")
        local outputDir = path.join(os.projectdir(), "Src/Common/DB/AutoGen")

        -- 增量：DDL .sql 文件比 .gen.h 新才重新生成
        local genFiles = os.files(path.join(outputDir, "*.gen.h"))
        local dirty = #genFiles == 0
        if not dirty then
            local oldestGen = os.mtime(genFiles[1])
            for _, f in ipairs(genFiles) do
                oldestGen = math.min(oldestGen, os.mtime(f))
            end
            for _, sqlF in ipairs(os.files(path.join(sqlDir, "*.sql"))) do
                if os.mtime(sqlF) > oldestGen then
                    dirty = true
                    break
                end
            end
        end

        if dirty then
            os.vrunv("python", {genScript, "--sql-dir", sqlDir, "--output", outputDir})
            cprint("${color.success}[db] .gen.h 已更新")
        end
    end)

target("CommonDB")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_headerfiles("AutoGen/*.gen.h")
    add_deps("CommonCore", "CommonQueue", "CommonLog", "LibPQ")
    add_rules("db_gen")
