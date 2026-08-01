# AOT（ScriptLayer_04_AOT）

> ⚠️ **本篇的 §1「构建 daslang 工具」+ §5「方案 A（stock daslang 生成）」已被
> `ScriptLayer_04b_AOT_SafeHost.md` 取代。** stock daslang 工具**看不到项目 native 模块
> （Common/World）**，对本项目脚本 AOT 会编译失败或 aotHash 漂移。请以 04b 的「自建生成宿主
> AotGen」为准。本篇的 §2/§3（Log 移公共头 + `aotRequire`）、§4（canonical policy）、
> §6（命中诊断）仍然有效，04b 在其上补齐了正确的生成宿主与 IV 安全修复。
>
> 本篇给出 AOT 相关的模块侧改造：`DasCommonModule::aotRequire`、Log 绑定函数移公共头、
> canonical policy、命中诊断。修复审查发现的 AOT-3/4/5/6。
>
> 承接 `ScriptLayer_00` / `02` / `03`，配合 `04b`。API 均对 `ThirdParty/daScript` 核实。

---

## 0. AOT 原理与现状

**原理**（已核实）：
- 构建期：`daslang utils/aot/main.das -- -aot in.das out.cpp` 把 `.das` 转 `.cpp`。生成的 `.cpp` 尾部含 `static AotListBase impl(registerAotFunctions);`——进程启动即自注册进全局 AOT 库（`ast.cpp:21-45`），**宿主无需手写任何 register 调用**。
- 运行期：`CodeOfPolicies::aot=true` 时，`simulate` 末尾自动 `linkCppAot(ctx, getGlobalAotLibrary(), logs)`（`ast_simulate.cpp:3803,3869`），按每函数 `aotHash` 把解释节点替换为原生函数指针。
- 命中判定：`ctx.findFunction("x")->aot == true`。

**现状**（审查）：`MakePolicies()` 只设了 `p.aot=true`，别的全无——AOT 库空、无生成 `.cpp`、无构建步骤、模块无 `aotRequire`。Release 静默 100% 解释执行。本篇全部补齐。

**前置约束**（aotHash 漂移，AOT-6）：生成端与运行端的 `.das` 源、全部传递依赖、daslang 版本、影响 codegen 的 policy、甚至路径分隔符（`/` vs `\` 进 SIM 树）必须一致，否则 `aotHash` 不匹配 → 该函数回退解释器（`fail_on_no_aot=false` 时静默）。所以必须定义**单一 canonical policy + 模块集**供两端共用。

---

## 1. 构建 daslang 工具（AOT-2）

`ThirdParty/daslang.lua` 末尾新增一个可执行 target，编 `utils/daScript/main.cpp` + 链 `libDaScript`。它同时是 AOT 生成器宿主与（可选的）`.dabin` 离线生成器宿主。

```lua
-- ThirdParty/daslang.lua 末尾追加
-- =============================================================================
-- daslang — CLI 可执行（AOT 代码生成 / dabin 离线生成 用）
--   对应 CMake daScript 可执行 target。仅构建期工具，不进发布运行时依赖。
-- =============================================================================
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
```

- daslang 版本与 `libDaScript` 同源，天然一致（无 v93 漂移）。
- 该 target 仅构建期用，不进发布包运行时依赖。

验证：`xmake build daslang` 后，`xmake run daslang -- <script.das>` 能跑脚本即可。

---

## 2. Log 绑定函数移公共头（AOT-4）

**问题**：`DasCommonModule.cpp` 里 `LogInfo/LogWarn/LogError` 是 `static void`（TU 局部），且在 `namespace MMO`。AOT 生成的 `.cpp` 会发射**无限定** `LogInfo(...)` 调用，既因 static 内部链接、又因命名空间，无法解析——编译不过。

**修复**：移到公共头的非 static 函数，绑定时用**全限定 cppName**。

新建 `Src/ScriptEngine/Module/DasCommonBinds.h`：

```cpp
#pragma once
#include "daScript/simulate/simulate.h"
#include "daScript/simulate/debug_info.h"

namespace MMO
{
    // 脚本绑定的日志函数——非 static、有头声明，使 AOT 生成的 .cpp 能链接到它们。
    // AOT 侧以全限定名 "MMO::LogInfo" 调用（见 DasCommonModule::aotRequire）。
    void LogInfo(const char *text, das::Context *ctx, das::LineInfoArg *at);
    void LogWarn(const char *text, das::Context *ctx, das::LineInfoArg *at);
    void LogError(const char *text, das::Context *ctx, das::LineInfoArg *at);
} // namespace MMO
```

`Src/ScriptEngine/Module/DasCommonModule.cpp` 改为定义这些非 static 函数（去掉 `static`，实现不变），并 `#include "DasCommonBinds.h"`：

```cpp
#include "DasCommonModule.h"
#include "DasCommonBinds.h"
#include "Common/Log/Log.h"
#include "daScript/ast/ast.h"
#include "daScript/daScript.h"
#include "daScript/simulate/debug_info.h"
#include "daScript/simulate/simulate.h"

namespace MMO
{
    void LogInfo(const char *text, das::Context *ctx, das::LineInfoArg *at)
    {
        if (nullptr != at && nullptr != at->fileInfo)
        {
            Log::At(ELogLevel::Info, at->fileInfo->name.c_str(), at->line, "{}",
                    nullptr != text ? text : "(null)");
        }
        else { Log::Info("{}", nullptr != text ? text : "(null)"); }
    }

    void LogWarn(const char *text, das::Context *ctx, das::LineInfoArg *at)
    {
        if (nullptr != at && nullptr != at->fileInfo)
        {
            Log::At(ELogLevel::Warn, at->fileInfo->name.c_str(), at->line, "{}",
                    nullptr != text ? text : "(null)");
        }
        else { Log::Warn("{}", nullptr != text ? text : "(null)"); }
    }

    void LogError(const char *text, das::Context *ctx, das::LineInfoArg *at)
    {
        if (nullptr != at && nullptr != at->fileInfo)
        {
            Log::At(ELogLevel::Error, at->fileInfo->name.c_str(), at->line, "{}",
                    nullptr != text ? text : "(null)");
        }
        else { Log::Error("{}", nullptr != text ? text : "(null)"); }
    }
    // ... DasCommonModule 定义见下 §3 ...
}
```

绑定时 cppName 用全限定 `"MMO::LogInfo"`（AOT 发射的调用即 `MMO::LogInfo(...)`，可解析）：

```cpp
// DasCommonModule::Build() 内，绑定改为全限定 cppName
das::addExtern<DAS_BIND_FUN(LogInfo)>(*this, lib, "LogInfo",
        das::SideEffects::modifyExternal, "MMO::LogInfo")->args({"text", "ctx", "at"});
das::addExtern<DAS_BIND_FUN(LogWarn)>(*this, lib, "LogWarn",
        das::SideEffects::modifyExternal, "MMO::LogWarn")->args({"text", "ctx", "at"});
das::addExtern<DAS_BIND_FUN(LogError)>(*this, lib, "LogError",
        das::SideEffects::modifyExternal, "MMO::LogError")->args({"text", "ctx", "at"});
```

---

## 3. `DasCommonModule::aotRequire`（AOT-3）

**问题**：`DasCommonModule` 没 override `aotRequire()` → 默认 `no_aot`。后果**极重**：任何脚本 `require Common`，AOT 生成器遇到 no_aot 模块会**整程序禁用 AOT**（`aot_cpp.das:4207` 写 `// AOT disabled due to this module`）。

**修复**：override 返回 `ModuleAotType::cpp`，并 `tw <<` 绑定函数所在头的 `#include`。参照 `dasStdDlg.cpp:44`。

`Src/ScriptEngine/Module/DasCommonModule.h`（完整，含 aotRequire 声明；删掉死成员 `_host`/`_timingWheel`，见 F10）：

```cpp
#pragma once
#include "daScript/ast/ast.h"
#include "daScript/daScriptModule.h"
#include "daScript/misc/string_writer.h"

namespace MMO
{
    class DasCommonModule : public das::Module
    {
    public:
        DasCommonModule();
        void Build();

        // AOT：声明本模块可 AOT，并 emit 绑定函数所在头的 #include（AOT-3）
        das::ModuleAotType aotRequire(das::TextWriter &tw) const override;
    };
} // namespace MMO
```

`DasCommonModule.cpp` 里实现 `aotRequire` 与 `Build`：

```cpp
    DasCommonModule::DasCommonModule() : das::Module("Common") {}

    void DasCommonModule::Build()
    {
        das::ModuleLibrary lib(this);
        lib.addBuiltInModule();   // 依赖 '$' 已注册（引擎 Initialize 里 PULL_ALL_DEFAULT_MODULES 先行）

        das::addExtern<DAS_BIND_FUN(LogInfo)>(*this, lib, "LogInfo",
                das::SideEffects::modifyExternal, "MMO::LogInfo")->args({"text", "ctx", "at"});
        das::addExtern<DAS_BIND_FUN(LogWarn)>(*this, lib, "LogWarn",
                das::SideEffects::modifyExternal, "MMO::LogWarn")->args({"text", "ctx", "at"});
        das::addExtern<DAS_BIND_FUN(LogError)>(*this, lib, "LogError",
                das::SideEffects::modifyExternal, "MMO::LogError")->args({"text", "ctx", "at"});
    }

    das::ModuleAotType DasCommonModule::aotRequire(das::TextWriter &tw) const
    {
        // 让 AOT 生成的 .cpp include Log 绑定函数的头（相对 daslang 工作目录的路径，见 §5 注意）
        tw << "#include \"ScriptEngine/Module/DasCommonBinds.h\"\n";
        return das::ModuleAotType::cpp;
    }
```

> 服务专用模块（`WorldModule`/`SocialModule`）未来也必须同样 override `aotRequire` 并 emit 各自绑定头（含生成的 `*.gen.cpp` 里 Proto 绑定所需的头），否则含它们的程序 AOT 被禁用。本轮不实现，仅记此约束。

---

## 4. canonical policy（AOT-6）

生成端（daslang 工具）与运行端（`DasLangEngine::MakePolicies`）的 policy 必须一致。daslang aot 工具的 `updateCOP` 已设 `aot=false, aot_module=true, fail_on_lack_of_aot_export=true`（生成端固定）；运行端我们控制 `MakePolicies`。要点：

- **路径归一**：`FsFileAccess` 传入的脚本路径统一用正斜杠 `/`，两端一致（Windows 上尤其注意）。
- **模块集一致**：生成与运行都经同一套 `IDasLangModuleProvider::CreateModules` + `DasCommonModule`。若用独立 daslang 工具生成，需让它加载相同模块——最稳妥是**用同一宿主初始化路径**（见 §5 方案 B）。
- **Develop/Release policy 差异**：Develop 有 `debugger=true`，会改变 codegen → aotHash 不同。所以 **AOT 只在 Release 生成与运行**；Develop 不碰 AOT。

运行端 `MakePolicies`（02 篇已给）Release 分支：

```cpp
p.aot            = true;
p.fail_on_no_aot = false;   // 未命中回退解释器（补丁场景必需）
```

---

## 5. xmake AOT 生成规则

### 方案 A（推荐起步）：用 daslang 工具批量生成

在 World（或含脚本的服务）xmake 里加一个 rule，构建期把脚本转 `.cpp` 并纳入编译。用 `before_build` + `depend.on_changed`（工具或源改了才重生成）。

```lua
-- Src/World/xmake.lua 追加
rule("das_aot")
    before_build(function (target)
        import("core.project.depend")
        -- daslang 工具产物路径（target daslang 的输出）
        local dasexe = path.join(target:targetdir(), "daslang" .. (is_plat("windows") and ".exe" or ""))
        local dasroot = path.join(os.projectdir(), "Script")
        local aotDir  = path.join(target:autogendir(), "aot")
        os.mkdir(aotDir)

        -- 需要 AOT 的脚本清单（入口 + 业务；daslib 由工具自动展开）
        local scripts = { "main.das", "Handlers.das", "HandlerRegistry.das" }
        for _, rel in ipairs(scripts) do
            local dasfile = path.join(dasroot, rel)
            local outcpp  = path.join(aotDir, rel:gsub("[/\\]", "_"):gsub("%.das$", "") .. ".das.cpp")
            depend.on_changed(function ()
                os.vrunv(dasexe, {
                    path.join(os.projectdir(), "ThirdParty/daScript/utils/aot/main.das"),
                    "--", "-aot", dasfile, outcpp
                })
                cprint("${color.success}[aot] %s -> %s", rel, path.filename(outcpp))
            end, {files = {dasfile, dasexe}, dependfile = outcpp .. ".d"})
            target:add("files", outcpp)
        end
    end)

-- WorldServer target 上（Release 才启用 AOT）
target("WorldServer")
    -- ...原有配置...
    if is_mode("release") then
        add_deps("daslang")          -- 先构建 daslang 工具
        add_rules("das_aot")
    end
```

> **注意**（AOT-6 路径归一）：`aotRequire` 里 emit 的 `#include "ScriptEngine/Module/DasCommonBinds.h"` 与 `.gen.cpp` include，需保证生成的 `.cpp` 编译时该相对路径可解析——把 `Src/` 加进 include 目录（`add_includedirs("$(projectdir)/Src")`），或在 aotRequire 里 emit 绝对/项目相对路径。生成的 `.das.cpp` 里所有 native 依赖头都靠各模块 `aotRequire` emit，务必齐全。

### 方案 B（更稳，后续）：宿主内生成，保证模块集一致

用 daslang 工具生成有一个隐患：工具的模块集与运行时的 `IDasLangModuleProvider` 可能不完全一致，导致 aotHash 漂移。更稳的是写一个**小的构建期宿主程序**（复用 `DasLangEngine` 的模块初始化 + `CodeOfPolicies`），调 daScript 的 AOT 生成 API 输出 `.cpp`。本轮先用方案 A 打通；若遇 `error[50101] AOT link failed`，再切方案 B。

---

## 6. 命中诊断（AOT-5）

`fail_on_no_aot=false` 让 AOT 链接失败**完全静默**——你无法知道到底跑没跑 AOT。**bring-up 期必须加诊断**，否则「AOT 优化版」可能全程解释执行而无人察觉。

在 `DasLangEngine::SimulateImage` 成功后（Release 模式）加一段统计：

```cpp
// DasEngine.cpp，SimulateImage 成功后（可加个 LogAotCoverage 私有方法）
#ifdef MMO_AOT_DIAG
    if (_cfg.mode == EScriptMode::Release)
    {
        int total = 0, aot = 0;
        for (int i = 0, n = img.ctx->totalFunctions; i < n; ++i)
        {
            if (das::SimFunction *fn = img.ctx->getFunction(i))
            {
                ++total;
                if (fn->aot) { ++aot; }
            }
        }
        Log::Info("AOT coverage: {}/{} functions native", aot, total);
    }
#endif
```

- bring-up 阶段建议临时 `p.fail_on_no_aot = true`：任何未命中直接编译报错，逼你补全生成/对齐 policy。全部命中后再改回 `false`（发布必须 `false`，否则热补丁里改动的函数 aotHash 不匹配会直接失败）。
- 稳定后用 `MMO_AOT_DIAG` 宏控制上面的覆盖率日志，发布可关。

---

## 7. 发布形态与热补丁的配合

- **发布包**：`WorldServer(.exe)` 含 AOT 生成的 `*.das.cpp`（原生速度）+ 完整 daScript 编译器；`Script/*.dabin`（03 篇，无明文）。
- **AOT 基线**：启动加载 `.dabin` → `simulate` 按 aotHash 命中 AOT 函数，全速跑。
- **热补丁**：下发新 `.dabin` → `RequestReload` → 重新 simulate。**被补丁改动的函数** aotHash 不匹配 → 自动回退解释器执行；**未改动的函数**仍走 AOT。于是线上小补丁改逻辑无需整包重编，性能仅补丁函数降为解释级。
- 这正是 `fail_on_no_aot=false` 在发布版必须为 false 的原因。

---

## 8. 落地顺序与验证

1. **构建工具**：加 `daslang` target，`xmake build daslang` 通过。
2. **模块侧**：`DasCommonBinds.h` + Log 函数去 static + 全限定 cppName + `DasCommonModule::aotRequire`。先确认非 AOT 的 Release 仍能正常编译运行。
3. **生成规则**：加 `das_aot` rule（Release），`xmake f -m release && xmake build WorldServer`，确认生成 `.das.cpp` 且编译链接通过。
4. **诊断**：开 `MMO_AOT_DIAG` + 临时 `fail_on_no_aot=true`，启动看 `AOT coverage: N/N`（应全命中）。若报 `error[50101]`：检查 policy/模块集/路径分隔符（AOT-6）。
5. **回退验证**：改回 `fail_on_no_aot=false`；打一个改动某 handler 的 `.dabin` 补丁热重载，确认该函数走解释器、其余仍 AOT、行为正确。

---

## 9. 三支柱协同小结

| | Develop | Release 启动 | Release 热补丁 |
|---|---|---|---|
| 脚本来源 | `.das` 源码 | `.dabin`（03 篇） | 下发 `.dabin` |
| 编译 | 全量 | 反序列化（省 parse/infer） | 反序列化 |
| 执行 | 解释 + debugger | AOT 命中全速（04 篇） | 改动函数解释、其余 AOT |
| 迭代 | 文件监视热重载（02 篇） | — | `RequestReload` swap（02 篇） |

三支柱正交叠加：**02 热重载**给开发期与线上补丁的切换机制、**03 .dabin**给无明文的分发/补丁载体、**04 AOT**给基线性能。至此脚本层底层组件完备，World/Social 业务模块按 §1(01 篇) 的 provider 约定后续拼装即可。
