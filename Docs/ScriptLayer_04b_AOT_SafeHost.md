# AOT 更稳方案：项目自建生成宿主（ScriptLayer_04b）

> 本篇是 `ScriptLayer_04_AOT.md` 的修订与深化，针对「用 stock daslang 工具生成 AOT」的
> **根本性缺陷**给出更稳、更安全的方案。所有机制均对 `ThirdParty/daScript` 源码核实。
>
> 结论先行：**不能用 stock `daslang` 工具对本项目脚本做 AOT**，必须自建一个链接了项目
> native 模块的生成宿主。下面解释为什么，并给出可照抄的完整实现。

---

## 1. 为什么 stock daslang 工具不行（根本缺陷）

`ThirdParty/daScript/utils/aot/main.das` 与 `utils/daScript/main.cpp` 的 AOT 路径，编译目标脚本时只 `require daslib/*`，**不注册任何项目 C++ native 模块**（`Common`/`World`）。

而你的脚本 `require Common`、handler 用 `MoveReq`/`EMsgID`——这些类型/枚举/绑定函数由 `DasCommonModule`/`WorldModule` 在 C++ 侧注册。用 stock 工具 AOT 会有两种失败：

1. **编译直接失败**：`require Common` 找不到模块 → `error: module 'Common' not found`。
2. **即使勉强过，aotHash 全漂移**：AOT 匹配靠每函数语义哈希（own hash + 传递依赖 hash）。生成端模块集与运行端不同 → 哈希不同 → 运行期一个 AOT 函数都链接不上，全部静默回退解释器（`fail_on_no_aot=false`），"AOT 优化版"名存实亡。

**核实**：`utils/daScript/main.cpp:80-140 aot_compile()` 里，编译目标脚本的 `CodeOfPolicies`（`getPolicies()`）不含任何项目模块注册；`daslib/aot_cpp.das` 的 `aot()` 通过**全局模块环境**解析 `require`——环境里没有 `Common`，就找不到。

> daScript 官方 `test_aot` 也不用裸工具：它的 AOT 生成宿主是链接了被测模块的自定义可执行（`tests/aot/` + `libDaScriptAot`），生成的 `.cpp` 与宿主编在一起。我们照此做。

---

## 2. 核心洞察：AOT 生成只需「同一个模块环境」

`daslib/aot_cpp.das` 的 `aot(input, ...)` 内部走的是标准 `compileDaScript` + 遍历 AST 发射 C++。它解析 `require Common` 时查的是**全局 daScriptEnvironment 的模块链表**（`Module::require` 扫 `daScriptEnvironment::getBound()->modules`）。

所以：**只要在调 `aot()` 之前，让本进程像运行期一样把 `DasCommonModule`/`WorldModule` 注册进全局环境**，stock 的 `aot()` 流程就能解析到它们，且用的是同一套模块 → aotHash 与运行期一致。

这就是「自建生成宿主」的全部要义：**一个小 main，先做与 `DasLangEngine::Initialize` 相同的模块注册，再驱动 daScript 的 AOT 生成。**

---

## 3. 方案对比

| | 方案 A：stock daslang | 方案 B：自建生成宿主（推荐） |
|---|---|---|
| 能否解析 `require Common` | ❌ 不能 | ✅ 能 |
| aotHash 与运行期一致 | ❌ 漂移 | ✅ 一致（同模块集 + 同 policy） |
| 适用 | 无 native 依赖的纯脚本 | 本项目（脚本依赖 native 模块） |
| 实现成本 | 已有 daslang target | +1 个小可执行 + 复用引擎初始化 |

**本项目必须走 B。** 你现在 `daslang.lua` 里加的 `daslang` target 保留（当"能跑脚本的验证器"有用），但 AOT 生成另起 `AotGen` target。

---

## 4. 自建生成宿主 `Tools/AotGen/AotGen.cpp`（可照抄）

设计要点：
- 复用 `DasLangEngine` 的模块注册路径（**同源**，杜绝模块集分叉）。
- policy 用**共享函数** `MakeScriptPolicies` 生成（生成端 = 运行端，除 aot 标志外逐字段一致）。
- 通过 C++ API `Program::aotCpp(ctx, logs, cross_platform)`（`ast.h:1747`）直接发射，省去再编译 `daslib/aot_cpp.das` 的中间层。

### 4.1 先抽出共享 policy（引擎与生成宿主共用）

在 `DasEngine.h/.cpp` 暴露一个静态工厂（或单独 `DasPolicies.h`）：

```cpp
// DasEngine.h，public static
static das::CodeOfPolicies MakeScriptPolicies(EScriptMode mode, bool forAotGen);
```

```cpp
// DasEngine.cpp
das::CodeOfPolicies DasLangEngine::MakeScriptPolicies(EScriptMode mode, bool forAotGen)
{
    das::CodeOfPolicies p;
    p.threadlock_context = true;
    p.persistent_heap    = true;
    p.rtti               = true;
    if (forAotGen)
    {
        // 生成端：aot=false + aot_module=true（照 utils/daScript/main.cpp:getPolicies）
        p.aot                          = false;
        p.aot_module                   = true;
        p.fail_on_lack_of_aot_export   = true;
    }
    else if (mode == EScriptMode::Release)
    {
        p.aot            = true;
        p.fail_on_no_aot = false;   // 运行期：未命中回退解释器（补丁场景必需）
    }
    else
    {
        p.debugger = true;          // Develop
    }
    return p;
}
```

> **关键不变量**：除 `aot`/`aot_module`/`debugger`/`fail_on_*` 这些标志外，`threadlock_context`/`persistent_heap`/`rtti` 等**影响 codegen 的项，两端必须逐字段相同**，否则 aotHash 变。把它们收在这一个函数里就是为了防漂移。`MakePolicies()`（运行期用）内部改为 `return MakeScriptPolicies(_cfg.mode, false);`。

### 4.2 生成宿主

```cpp
// Tools/AotGen/AotGen.cpp
// 用法: AotGen <dasRoot> <script.das> <out.cpp>
// 作用: 用与运行期相同的模块集 + policy，把 script.das 转成 AOT C++（out.cpp）。
#include "ScriptEngine/DasEngine.h"
#include "ScriptEngine/Module/DasCommonModule.h"
// 若要 AOT World 脚本，这里 include 并注册 WorldModule 及其 Proto 绑定
// #include "World/DasModule/WorldDasModule.h"

#include "daScript/daScript.h"
#include "daScript/daScriptModule.h"
#include "daScript/simulate/fs_file_info.h"

#include <cstdio>
#include <memory>

// F2 同理：默认模块声明放文件作用域（全局），避免 namespace 污染
DECLARE_ALL_DEFAULT_MODULES;

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: AotGen <dasRoot> <script.das> <out.cpp>\n");
        return 1;
    }
    const char *dasRoot = argv[1];
    const char *inDas   = argv[2];
    const char *outCpp  = argv[3];

    das::setDasRoot(dasRoot);
    PULL_ALL_DEFAULT_MODULES;

    // ★ 与 DasLangEngine::Initialize 完全相同的模块注册（同源，保证 aotHash 一致）
    auto common = std::make_unique<MMO::DasCommonModule>();
    common->Build();
    // WorldModule：与运行期 provider->CreateModules 等价——建 WorldModule + RegisterAllProtoMessageTypes
    // auto world = std::make_unique<MMO::WorldModule>(); world->Build(); ...

    das::Module::Initialize();

    // FsFileAccess + daslib
    auto fa = das::make_smart<das::FsFileAccess>();
    fa->introduceDaslib();

    das::ModuleGroup libGroup;
    // 若 WorldModule 需要进 group：libGroup.addModule(world.get());
    // Common 无需（native 模块经环境全局表自动解析进 libGroup）

    das::TextWriter logs;
    das::CodeOfPolicies cop =
        MMO::DasLangEngine::MakeScriptPolicies(MMO::EScriptMode::Release, /*forAotGen*/ true);

    auto program = das::compileDaScript(inDas, fa, logs, libGroup, cop);
    if (!program || program->failed())
    {
        std::fprintf(stderr, "AotGen compile failed: %s\n%s\n", inDas, logs.str().c_str());
        return 2;
    }

    // simulate 到一个 Context，再发射 AOT C++
    auto ctx = std::make_unique<das::Context>(program->getContextStackSize());
    if (!program->simulate(*ctx, logs))
    {
        std::fprintf(stderr, "AotGen simulate failed: %s\n%s\n", inDas, logs.str().c_str());
        return 3;
    }

    das::TextWriter cpp;
    program->aotCpp(*ctx, cpp, /*cross_platform*/ false);   // ast.h:1747

    // aotCpp 只写函数体；完整文件还需 prologue（见 §4.3）。这里给出最小可用：
    FILE *fp = std::fopen(outCpp, "wb");
    if (!fp) { std::fprintf(stderr, "AotGen: cannot open %s\n", outCpp); return 4; }
    std::fputs(cpp.str().c_str(), fp);
    std::fclose(fp);
    return 0;
}
```

### 4.3 关于 prologue（重要，别漏）

`Program::aotCpp` 发射的是函数体 + `AotListBase` 注册尾，但一个可编译的 `.cpp` 还需要**头部 include 段**（`#include "daScript/misc/performance_time.h"` 等 + 各 native 模块经 `aotRequire` emit 的 include）。daScript 的 daslib `aot()` 会自动拼这段 prologue；直接用 C++ `aotCpp` 则需你补，或**更省心地走 daslib `aot()` 路径**（见 §5）。

**两条子路线**：
- **B1（C++ API，最少中间层）**：`program->aotCpp(...)` + 手拼 prologue。prologue 内容参照 `daslib/aot_cpp.das` 里 `aotProlog`/生成的固定头。控制力最强，但要维护 prologue。
- **B2（复用 daslib aot()，推荐）**：生成宿主里像 `utils/daScript/main.cpp:aot_compile` 那样，编译 `daslib/aot_cpp.das` → 找 `aot` 函数 → `evalWithCatch(aotFn, {input, paranoid, cross_platform, &cop})`，让 daslib 自己拼完整文件。**关键差别**：你的宿主在调之前已注册了 `Common`/`World` 模块进全局环境，所以 daslib 的 `aot()` 解析 `require Common` 就能成功。这条最省事且 prologue 全自动。

> **推荐 B2**：改动最小（照抄 `utils/daScript/main.cpp:80-140`），只是在其 `main` 初始化里加上 §4.2 的模块注册。等于「stock 工具 + 我们的模块注册」。

---

## 5. B2 生成宿主（推荐，照抄 + 加模块注册）

最实际的做法：复制 `ThirdParty/daScript/utils/daScript/main.cpp` 为 `Tools/AotGen/AotGen.cpp`，在其模块初始化处插入项目模块注册。核心只加这几行（其余照抄它的 `aot_compile`）：

```cpp
// AotGen.cpp，在 daScript 初始化之后、aot_compile 之前
DECLARE_ALL_DEFAULT_MODULES;   // 文件作用域

int main(int argc, char **argv)
{
    // ... 照抄 daScript main.cpp 的 argv 解析 / setDasRoot ...
    PULL_ALL_DEFAULT_MODULES;

    // ★★ 唯一的增量：注册项目 native 模块，令 aot() 能解析 require Common/World
    static auto common = std::make_unique<MMO::DasCommonModule>();
    common->Build();
    // static auto world = std::make_unique<MMO::WorldModule>(); world->Build();

    das::Module::Initialize();

    // ... 照抄 aot_compile(aot_files, ...) ...
}
```

`Tools/AotGen/xmake.lua`：

```lua
target("AotGen")
    set_kind("binary")
    add_files("*.cpp")
    add_deps("ScriptEngine")     -- 带来 DasCommonModule + libDaScript
    -- 若 AOT World 脚本，还要能注册 WorldModule + Proto 绑定：
    -- add_deps("Proto") + 把 Src/World/AutoGen/*.gen.cpp 纳入（或抽成独立静态库）
    add_deps("CommonCore", "CommonLog", "CommonTimer", "CommonCrypto")
```

> World 脚本的 AOT 有个现实约束：生成宿主要注册 `WorldModule` + `RegisterAllProtoMessageTypes`（消息类型/EMsgID），意味着 `AotGen` 要链接 World 侧的 Proto 绑定。若不想让 `AotGen` 依赖整个 `WorldServer`，把「WorldModule + Proto 绑定」抽成一个独立静态库（如 `WorldScriptModule`），`AotGen` 和 `WorldServer` 都依赖它。这也顺带解决了 R5 报告 C5 的 `WorldDasModule.cpp` 归属。

---

## 6. xmake 生成规则（用 AotGen，非 stock daslang）

### 6.1 AOT 的粒度：只列「入口脚本」，不列被 require 的文件

**关键**：AOT `main.das` 时，daScript 编译的是 `main.das` 的**整个 require 闭包**，发射器 `for_each_module_no_order`（`aot_cpp.das:3982/3988/4096`）会为**闭包内所有模块的所有函数**生成 stub。所以：

- **只 AOT 各服务的入口脚本**（World 的 `Script/World/main.das`）即可——`Handlers.das`/`MsgHandlerRegistry.das` 作为被 require 的 das 模块，其函数已被闭包覆盖。
- **绝不要**把 `Handlers.das`/`MsgHandlerRegistry.das` 单独列进 AOT 列表：它们要么是 `module` 文件（无 `main`）、要么会被当独立 entry 重编，产生**重复/重叠的 stub 或 aotHash 撞车**。
- daslib（`daslib/*`）是另一回事，走 `-aotlib`（`libDaScriptAot`）单独预编，与业务脚本无关。

一个入口脚本 → 一个 `.das.cpp`，覆盖整条 require 链。

### 6.2 入口列表来自 provider/config，不写死路径

入口脚本的「根」是各服务的入口（`IDasLangModuleProvider::MainScriptFile()` 已提供，或 `DasLangEngineConfig::mainFile`）。xmake rule 从一个**服务级配置**读，而非硬编码：

```lua
-- Src/World/xmake.lua（Release 才 AOT）
-- AOT 入口清单：只列本服务的【入口脚本】，其 require 闭包（Handlers / MsgHandlerRegistry / ...）
-- 会被 daScript 编译时自动纳入，无需也不应在此列出。
local kAotEntryScripts = { "World/main.das" }   -- 单一真相源；将来 Social 是 { "Social/main.das" }

rule("das_aot")
    before_build(function (target)
        import("core.project.depend")
        local aotgen  = path.join(target:targetdir(), "AotGen" .. (is_plat("windows") and ".exe" or ""))
        local dasroot = path.join(os.projectdir(), "Script")
        local aotDir  = path.join(target:autogendir(), "aot")
        os.mkdir(aotDir)

        for _, rel in ipairs(kAotEntryScripts) do
            local dasfile = path.join(dasroot, rel)
            local outcpp  = path.join(aotDir, rel:gsub("[/\\]", "_"):gsub("%.das$", "") .. ".das.cpp")
            depend.on_changed(function ()
                os.vrunv(aotgen, { dasroot, dasfile, outcpp })
                cprint("${color.success}[aot] %s (+require 闭包)", rel)
            end, {files = {dasfile, aotgen}, dependfile = outcpp .. ".d"})
            target:add("files", outcpp)
        end
    end)

target("WorldServer")
    if is_mode("release") then
        add_deps("AotGen")       -- 先构建生成宿主
        add_rules("das_aot")
        add_includedirs("$(projectdir)/Src")  -- 让生成的 .cpp 能 include 各模块 aotRequire emit 的头
    end
```

> `on_changed` 的 `files` 只列了入口 + AotGen——若 `Handlers.das` 改了但 `main.das` 没改，增量不会重生成。稳妥起见把整个 `Script/` 目录的 mtime 纳入依赖，或在 depfile 里让 AotGen 输出它实际读了哪些文件（daScript 编译期可拿 `getAllFiles`）。最简单：`files` 里加 `os.files(path.join(dasroot, "**.das"))`。

---

## 7. 安全性加固（"更安全"）

| 面 | 现状 | 加固 |
|---|---|---|
| 无源码明文 | `.dasbin` 是 AST，无明文 ✓ | 保持 |
| 加密 + 防篡改 | `Aes256Gcm`（AEAD）已实现 ✓ | 保持；GCM tag 即防篡改，无需叠 HMAC |
| **IV 唯一性** 🔴 | `Save` 用 `blobLen` 派生 IV | **必改**：见下 §7.1 |
| aotHash 校验 | 补丁改动函数回退解释器 ✓ | 发布必须 `fail_on_no_aot=false` |
| 密钥 | hex 内嵌配置 | 门槛级；要强保护需密钥服务器下发（后续） |
| bring-up 可观测 | `fail_on_no_aot=false` 静默 | bring-up 期临时 `true` + 覆盖率日志（§8） |

### 7.1 IV 唯一性必修（当前有安全缺陷）

`DasSerializer.cpp:128-131` 用 `blobLen` 派生 IV：

```cpp
uint8 iv[Aes256Gcm::kIvSize] = {0};
std::memcpy(iv, &blobLen, ...);   // ← 同密钥下，两个 blobLen 相同的脚本 IV 重用
```

**GCM 在同密钥下 IV 重用会彻底破坏机密性与完整性**（nonce-reuse 是 GCM 的致命误用）。`.dasbin` 是离线一次性产物，最简单稳妥：**每次生成随机 IV**（IV 已随 payload 存储，读取端从 payload 头取，无需改格式）：

```cpp
// Save() 里，替换 blobLen 派生 IV
uint8 iv[Aes256Gcm::kIvSize];
if (!Crypto::RandomBytes(iv, sizeof(iv)))   // 复用项目现有 CSPRNG（如 openssl RAND_bytes 封装）
{
    Log::Error("DasLangSerializer Save: RNG failed");
    return false;
}
```

> 项目 `Common/Crypto` 已依赖 openssl，`RAND_bytes` 现成。若还没有 `RandomBytes` 封装，加一个 4 行的即可。读取端 `Load` 已经是「从 payload 头取 iv[12]」，无需改动。

---

## 8. bring-up 诊断（AOT-5）

`fail_on_no_aot=false` 让 AOT 链接失败**完全静默**。落地阶段必须能看见命中率，否则"AOT 版"可能全程解释执行而无人察觉。

```cpp
// DasEngine.cpp，SimulateImage 成功后（Release）
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

- bring-up：临时把 `MakeScriptPolicies` 里 Release 分支设 `fail_on_no_aot = true`——任何未命中直接编译报错（`error[50101]`），逼你对齐生成/运行的模块集与 policy。
- 全命中后改回 `false`（发布必须 false：补丁改动函数 aotHash 不匹配要能回退解释器，否则热补丁直接失败）。

---

## 9. 落地顺序

1. 抽 `MakeScriptPolicies(mode, forAotGen)` 共享函数，`MakePolicies` 改为调它。
2. （若做 World AOT）把 WorldModule + Proto 绑定抽成独立静态库 `WorldScriptModule`。
3. 建 `Tools/AotGen/AotGen.cpp`（B2：照抄 daScript main.cpp + 加模块注册）+ `xmake.lua`。
4. `AotGen` 手动跑一次：`AotGen Script Script/World/main.das out.cpp`，确认生成的 `.cpp` 能被项目编译器编译。
5. 加 `das_aot` rule（Release），开 `MMO_AOT_DIAG` + 临时 `fail_on_no_aot=true`，`AOT coverage` 应全命中；报 50101 就查模块集/policy/路径分隔符。
6. 修 IV 随机化（§7.1）——加密发布前必做。
7. 改回 `fail_on_no_aot=false`，验证热补丁回退。

---

## 10. 一句话总结

**stock daslang 工具对本项目 AOT 会失败或哈希漂移；正确做法是自建一个「先注册 Common/World 模块、再复用 daScript aot() 流程」的生成宿主（`AotGen`），并把 policy 收敛到单一 `MakeScriptPolicies` 保证生成端与运行端逐字节一致。** 安全侧额外必修一项：`.dasbin` 加密的 IV 必须随机化，当前用 blobLen 派生存在 GCM nonce-reuse 缺陷。
