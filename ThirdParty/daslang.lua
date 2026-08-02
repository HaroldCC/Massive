--- @file daslang.lua
--- @brief daScript — 高性能脚本语言（按 CMake 分 target 编译）
---
--- 三个 target 对应 CMake 同名结构，保留上游命名：
---   libUriParser            — URI 解析 C 库（15 个 .c 文件）
---   libDaScript_runtime    — 核心运行库（parser/ast/builtin/misc/simulate，不含 fusion）
---   libDaScript            — fusion 层 + modules.cpp + daScriptC.cpp
---
--- 项目 C++ 代码只需 add_deps("libDaScript") 即可完整链接。

-- =============================================================================
-- 全局定义与规则
-- =============================================================================

--- 按 CMake SETUP_LIB_DEFINITIONS 逻辑：Release=2，Debug=0
local dasFusionMode = is_mode("release") and "2" or "0"

--- CMakeCommon.txt 预定义的全局编译定义
local commonDefines = {
    "SIZE_OF_VOID_P=8",
    "_CRT_SECURE_NO_WARNINGS",
}

--- 平台编译器标志，内联到每个 target 中
local function setupPlatformFlags()
    if is_plat("mingw") then
        add_cxflags("-fno-strict-aliasing")
    elseif is_plat("windows") then
        add_cxflags("/bigobj", "/EHa")
    elseif is_plat("linux") then
        add_cxflags("-fno-strict-aliasing")
    elseif is_plat("macosx") then
        add_cxflags("-fwrapv", "-Wno-c99-extensions",
                    "-Wno-unused-variable", "-Wno-deprecated-declarations",
                    "-Wno-missing-field-initializers", "-Wno-deprecated-copy")
    end
end

--- need_and_resolve 空头文件（对应 CMake NEED_MODULES_PATH）
--- fs_file_info.cpp 需要 #include "modules/external_resolve.inc"
local modulesIncDir = path.join(os.projectdir(), "ThirdParty/daScript/build/xmake_include")

-- =============================================================================
-- libUriParser — URI 解析 C 库
--
-- 对应 CMakeLists.txt:605-620
-- =============================================================================

target("libUriParser")
    set_kind("static")
    add_rules("Rules.ThirdParty")
    add_files("daScript/3rdparty/uriparser/src/*.c")
    add_sysincludedirs("daScript/3rdparty/uriparser/include", {public = true})
    add_defines("URIPARSER_BUILD_CHAR", "URI_STATIC_BUILD", {public = true})
    -- CMake 以 C++ 编译 uriparser（MSVC C 不支持 void* 隐式转换）
    set_languages("c++17")
    if is_plat("mingw") then
        add_defines("_GNU_SOURCE")
    end

-- =============================================================================
-- libDaScript_runtime — 核心运行库
--
-- 对应 CMakeLists.txt:1022-1027 (LIBDASCRIPT_RUNTIME_SRC)
-- 包含：parser/ast/builtin/misc/simulate/hal/runtime/formatter
-- 不包含：simulate_fusion* / modules.cpp / daScriptC.cpp（这些在 libDaScript）
--
-- CMake 依赖链：libDaScript_runtime → libUriParser
-- =============================================================================

target("libDaScript_runtime")
    set_kind("static")
    add_rules("Rules.ThirdParty")
    add_deps("libUriParser")

    -- 禁用第三方库警告
    set_warnings("none")

    -- ── 源文件（按 CMake 分组组织）──

    -- parser（预生成 .cpp，无需 flex/bison）
    add_files("daScript/src/parser/ds_parser.cpp")
    add_files("daScript/src/parser/ds_lexer.cpp")
    add_files("daScript/src/parser/ds2_parser.cpp")
    add_files("daScript/src/parser/ds2_lexer.cpp")
    add_files("daScript/src/parser/parser_impl.cpp")

    -- ast
    add_files("daScript/src/ast/ast.cpp")
    add_files("daScript/src/ast/ast_dispatch.cpp")
    add_files("daScript/src/ast/ast_interop.cpp")
    add_files("daScript/src/ast/ast_tls.cpp")
    add_files("daScript/src/ast/ast_visitor.cpp")
    add_files("daScript/src/ast/ast_generate.cpp")
    add_files("daScript/src/ast/ast_simulate.cpp")
    add_files("daScript/src/ast/ast_typedecl.cpp")
    add_files("daScript/src/ast/ast_match.cpp")
    add_files("daScript/src/ast/ast_module.cpp")
    add_files("daScript/src/ast/ast_print.cpp")
    add_files("daScript/src/ast/ast_program.cpp")
    add_files("daScript/src/ast/ast_infer_type.cpp")
    add_files("daScript/src/ast/ast_infer_type_report.cpp")
    add_files("daScript/src/ast/ast_infer_type_function.cpp")
    add_files("daScript/src/ast/ast_infer_type_helper.cpp")
    add_files("daScript/src/ast/ast_infer_type_op.cpp")
    add_files("daScript/src/ast/ast_infer_type_make.cpp")
    add_files("daScript/src/ast/ast_lint.cpp")
    add_files("daScript/src/ast/ast_allocate_stack.cpp")
    add_files("daScript/src/ast/ast_derive_alias.cpp")
    add_files("daScript/src/ast/ast_const_folding.cpp")
    add_files("daScript/src/ast/ast_block_folding.cpp")
    add_files("daScript/src/ast/ast_gc_collect.cpp")
    add_files("daScript/src/ast/ast_inscope_pod.cpp")
    add_files("daScript/src/ast/ast_unused.cpp")
    add_files("daScript/src/ast/ast_annotations.cpp")
    add_files("daScript/src/ast/ast_export.cpp")
    add_files("daScript/src/ast/ast_parse.cpp")
    add_files("daScript/src/ast/ast_validate.cpp")
    add_files("daScript/src/ast/ast_debug_info_helper.cpp")
    add_files("daScript/src/ast/ast_handle.cpp")
    add_files("daScript/src/ast/dyn_modules.cpp")

    -- builtin
    add_files("daScript/src/builtin/module_builtin.cpp")
    add_files("daScript/src/builtin/module_builtin_misc_types.cpp")
    add_files("daScript/src/builtin/module_builtin_runtime.cpp")
    add_files("daScript/src/builtin/module_builtin_runtime_sort.cpp")
    add_files("daScript/src/builtin/module_builtin_runtime_lockcheck.cpp")
    add_files("daScript/src/builtin/module_builtin_vector.cpp")
    add_files("daScript/src/builtin/module_builtin_vector_ctor.cpp")
    add_files("daScript/src/builtin/module_builtin_array.cpp")
    add_files("daScript/src/builtin/module_builtin_math.cpp")
    add_files("daScript/src/builtin/module_builtin_string.cpp")
    add_files("daScript/src/builtin/module_builtin_rtti.cpp")
    add_files("daScript/src/builtin/module_builtin_ast.cpp")
    add_files("daScript/src/builtin/module_builtin_ast_serialize.cpp")
    add_files("daScript/src/builtin/module_builtin_ast_flags.cpp")
    add_files("daScript/src/builtin/module_builtin_ast_annotations.cpp")
    add_files("daScript/src/builtin/module_builtin_ast_annotations_1.cpp")
    add_files("daScript/src/builtin/module_builtin_ast_annotations_2.cpp")
    add_files("daScript/src/builtin/module_builtin_ast_annotations_3.cpp")
    add_files("daScript/src/builtin/module_builtin_ast_adapters.cpp")
    add_files("daScript/src/builtin/module_builtin_uriparser.cpp")
    add_files("daScript/src/builtin/module_jit.cpp")
    add_files("daScript/src/builtin/module_builtin_fio.cpp")
    add_files("daScript/src/builtin/module_builtin_dasbind.cpp")
    add_files("daScript/src/builtin/module_builtin_network.cpp")
    add_files("daScript/src/builtin/module_builtin_debugger.cpp")
    add_files("daScript/src/builtin/module_builtin_jobque.cpp")
    add_files("daScript/src/builtin/module_file_access.cpp")

    -- misc
    add_files("daScript/src/misc/gc_node.cpp")
    add_files("daScript/src/misc/globals.cpp")
    add_files("daScript/src/misc/sysos.cpp")
    add_files("daScript/src/misc/string_writer.cpp")
    add_files("daScript/src/misc/memory_model.cpp")
    add_files("daScript/src/misc/job_que.cpp")
    add_files("daScript/src/misc/handle_registry.cpp")
    add_files("daScript/src/misc/free_list.cpp")
    add_files("daScript/src/misc/alloc_tracker.cpp")
    add_files("daScript/src/misc/alloc_tracker_fast_stack.cpp")
    add_files("daScript/src/misc/lexer_alloc_track.cpp")
    add_files("daScript/src/misc/uric.cpp")
    add_files("daScript/src/misc/format.cpp")
    add_files("daScript/src/misc/network.cpp")
    add_files("daScript/src/misc/alloc_tracker_overrides.cpp")

    -- hal
    add_files("daScript/src/hal/performance_time.cpp")
    add_files("daScript/src/hal/debug_break.cpp")
    add_files("daScript/src/hal/crash_handler.cpp")
    add_files("daScript/src/hal/project_specific.cpp")
    add_files("daScript/src/hal/project_specific_file_info.cpp")
    add_files("daScript/src/hal/project_specific_crash_handler.cpp")

    -- simulate
    add_files("daScript/src/simulate/hash.cpp")
    add_files("daScript/src/simulate/debug_info.cpp")
    add_files("daScript/src/simulate/runtime_string.cpp")
    add_files("daScript/src/simulate/runtime_array.cpp")
    add_files("daScript/src/simulate/runtime_table.cpp")
    add_files("daScript/src/simulate/runtime_profile.cpp")
    add_files("daScript/src/simulate/standalone_ctx_utils.cpp")
    add_files("daScript/src/simulate/simulate.cpp")
    add_files("daScript/src/simulate/simulate_exceptions.cpp")
    add_files("daScript/src/simulate/simulate_gc.cpp")
    add_files("daScript/src/simulate/simulate_tracking.cpp")
    add_files("daScript/src/simulate/simulate_visit.cpp")
    add_files("daScript/src/simulate/simulate_print.cpp")
    add_files("daScript/src/simulate/simulate_fn_hash.cpp")
    add_files("daScript/src/simulate/simulate_instrument.cpp")
    add_files("daScript/src/simulate/heap.cpp")
    add_files("daScript/src/simulate/data_walker.cpp")
    add_files("daScript/src/simulate/debug_print.cpp")
    add_files("daScript/src/simulate/json_print.cpp")
    add_files("daScript/src/simulate/json_scan.cpp")
    add_files("daScript/src/simulate/bin_serializer.cpp")
    add_files("daScript/src/simulate/fs_file_info.cpp")

    -- runtime
    add_files("daScript/src/runtime/context.cpp")

    -- formatter（CMake 将 dasFormatter 归入 LIBDASCRIPT_RUNTIME_SRC）
    add_files("daScript/utils/dasFormatter/fmt.cpp")
    add_files("daScript/utils/dasFormatter/formatter.cpp")
    add_files("daScript/utils/dasFormatter/helpers.cpp")
    add_files("daScript/utils/dasFormatter/ds_parser.cpp")

    -- ── include 路径 ──
    add_sysincludedirs("daScript/include", {public = true})
    add_sysincludedirs("daScript/3rdparty/uriparser/include", {public = true})
    add_sysincludedirs("daScript/3rdparty/fmt/include", {public = true})

    -- ── 编译定义 ──
    add_defines(commonDefines, {public = true})
    add_defines("DAS_ENABLE_DYN_INCLUDES=1", {public = true})
    add_defines(string.format("DAS_FUSION=%s", dasFusionMode), {public = true})
    if is_mode("release") then
        add_defines("DAS_NO_ASSERTIONS", {public = true})
    end

    -- ── 平台编译标志（按 CMakeCommon.txt）──
    if is_plat("mingw") then
        -- MinGW-w64 可能缺少 posix_memalign/malloc_usable_size，
        -- 定义 _MSC_VER 让 platform.h 走 _aligned_malloc 路径
        add_defines("_MSC_VER=1900")
    end
    setupPlatformFlags()

    -- ── need_and_resolve 空头文件（CMake 生成，被 fs_file_info.cpp include）──
    before_build(function(target)
        import("core.project.depend")
        local incFile = path.join(modulesIncDir, "modules", "external_resolve.inc")
        if not os.isfile(incFile) then
            os.mkdir(path.join(modulesIncDir, "modules"))
            io.open(incFile, "w"):close()
        end
    end)
    on_config(function(target)
        target:add("sysincludedirs", modulesIncDir)
    end)

-- =============================================================================
-- libDaScript — fusion 层 + 模块注册
--
-- 对应 CMakeLists.txt:1029-1033 (LIBDASCRIPT_COMPILER_SRC)
-- 包含：simulate_fusion* / daScriptC.cpp / modules.cpp
--
-- CMake 依赖链：libDaScript → libDaScript_runtime
-- =============================================================================

target("libDaScript")
    set_kind("static")
    add_rules("Rules.ThirdParty")
    add_deps("libDaScript_runtime")

    set_warnings("none")

    -- fusion 层（DAS_FUSION>=2 时生效，条件编译自动控制）
    add_files("daScript/src/simulate/simulate_fusion.cpp")
    add_files("daScript/src/simulate/simulate_fusion_op1.cpp")
    add_files("daScript/src/simulate/simulate_fusion_op1_return.cpp")
    add_files("daScript/src/simulate/simulate_fusion_ptrfdr.cpp")
    add_files("daScript/src/simulate/simulate_fusion_op2.cpp")
    add_files("daScript/src/simulate/simulate_fusion_op2_set.cpp")
    add_files("daScript/src/simulate/simulate_fusion_op2_bool.cpp")
    add_files("daScript/src/simulate/simulate_fusion_op2_bin.cpp")
    add_files("daScript/src/simulate/simulate_fusion_op2_vec.cpp")
    add_files("daScript/src/simulate/simulate_fusion_op2_set_vec.cpp")
    add_files("daScript/src/simulate/simulate_fusion_op2_bool_vec.cpp")
    add_files("daScript/src/simulate/simulate_fusion_op2_bin_vec.cpp")
    add_files("daScript/src/simulate/simulate_fusion_op2_scalar_vec.cpp")
    add_files("daScript/src/simulate/simulate_fusion_at.cpp")
    add_files("daScript/src/simulate/simulate_fusion_at_array.cpp")
    add_files("daScript/src/simulate/simulate_fusion_tableindex.cpp")
    add_files("daScript/src/simulate/simulate_fusion_misc_copy.cpp")
    add_files("daScript/src/simulate/simulate_fusion_call1.cpp")
    add_files("daScript/src/simulate/simulate_fusion_call2.cpp")
    add_files("daScript/src/simulate/simulate_fusion_if.cpp")
    add_files("daScript/src/simulate/simulate_fusion_tablewithhash.cpp")

    -- C API 和模块注册
    add_files("daScript/src/misc/daScriptC.cpp")
    add_files("daScript/src/builtin/modules.cpp")

    -- 补充：编译链路所需但原本未收录的源文件
    add_files("daScript/src/ast/ast_optimize.cpp")         -- optimizeProgram
    add_files("daScript/src/ast/ast_gc_report.cpp")        -- gcReportHistogram/gcStageReportDelta
    add_files("daScript/src/ast/ast_escape_analysis.cpp")  -- escapeAnalysis/scopeFreeOptimization
    add_files("daScript/src/misc/das_common.cpp")          -- starts_with
    add_files("daScript/src/simulate/runtime_iterator.cpp") -- PointerDimIterator::first/next/close

    -- ── include 路径 ──
    add_sysincludedirs("daScript/include", {public = true})

    -- ── 编译定义 ──
    add_defines(commonDefines, {public = true})
    add_defines("DAS_ENABLE_DYN_INCLUDES=1", {public = true})
    add_defines(string.format("DAS_FUSION=%s", dasFusionMode), {public = true})
    if is_mode("release") then
        add_defines("DAS_NO_ASSERTIONS", {public = true})
    end

    -- ── 平台链接（Threads::Threads 在 UNIX 上 = dl + pthread）──
    if is_plat("windows") then
        add_syslinks("dbghelp", "ws2_32")
    elseif is_plat("linux") then
        add_syslinks("dl", "pthread")
    elseif is_plat("macosx") then
        add_syslinks("pthread")
    end

    -- ── 平台编译标志 ──
    if is_plat("mingw") then
        add_defines("_GNU_SOURCE")
    end
    setupPlatformFlags()

    -- ── need_and_resolve 空头文件 ──
    before_build(function(target)
        import("core.project.depend")
        local incFile = path.join(modulesIncDir, "modules", "external_resolve.inc")
        if not os.isfile(incFile) then
            os.mkdir(path.join(modulesIncDir, "modules"))
            io.open(incFile, "w"):close()
        end
    end)
    on_config(function(target)
        target:add("sysincludedirs", modulesIncDir)
    end)

target("daslang")
    set_kind("binary")
    add_rules("Rules.ThirdParty")
    add_deps("libDaScript")
    set_warnings("none")

    add_files("daScript/utils/daScript/main.cpp")

    add_sysincludedirs("daScript/include")
    add_defines(commonDefines)
    add_defines("DAS_ENABLE_DYN_INCLUDES=1")
    add_defines(string.format("DAS_FUSION=%s", dasFusionMode))
    if is_mode("release") then
        add_defines("DAS_NO_ASSERTIONS")
    end

    if is_plat("windows") then
        add_syslinks("dbghelp", "ws2_32")
    elseif is_plat("linux") then
        add_syslinks("dl", "pthread")
    elseif is_plat("macosx") then
        add_syslinks("pthread")
    end
    if is_plat("mingw") then
        add_defines("_GNU_SOURCE")
    end
    setupPlatformFlags()

    on_config(function(target)
        target:add("sysincludedirs", modulesIncDir)
    end)

    before_build(function(target)
        import("core.project.depend")
        local incFile = path.join(modulesIncDir, "modules", "external_need.inc")
        if not os.isfile(incFile) then
            os.mkdir(path.join(modulesIncDir, "modules"))
            io.open(incFile, "w"):close()
        end
    end)
