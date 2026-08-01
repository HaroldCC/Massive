# 脚本层实现审查报告（R5）

> 审查对象：用户当前工作树（未提交）的 ScriptEngine / World / Script 实现。
> 每条结论均对 `ThirdParty/daScript` 源码或 gen2 语法核实。
> 优先级：🔴 编译不过/崩溃 → 🟡 功能/一致性 → 🟢 清理。

---

## 先说结论

骨架方向对，`DasSerializer.cpp` 质量高（header 逐字段大端、`try/catch` 兜 noexcept、加密层、回退矩阵都对），`IDasHost` 精简对了，热重载 `PollReload`/`DoSwap`/编译失败保留旧 image 的安全网也对。剩下的是一批**具体 bug**，集中在 das 语法、初始化时序、AOT 收尾。

---

## 🔴 A. 编译不过 / 会崩溃

### A1. `Script/Common/MsgHandlerRegistry.das` 多处 daslang 语法错误（整文件编译不过）

| 行 | 错误 | 修正 |
|---|---|---|
| 6-9 | `require daslib.ast` 等用**点**分隔 | `require daslib/ast`（斜杠）。核实：daslib 全部用 `require daslib/xxx`（`ast_boost.das:14`） |
| 16 | `daf private MsgNameToMsgID` —— **`daf` 拼写** | `def private` |
| 22 | `charactor_at` 拼写（32 行又写对成 `character_at`） | 统一 `character_at` |
| 13 | `table<uint, function<...>>` 用**逗号** | gen2 table 类型参数用 `;`：`table<uint; function<(sessionID : uint; msgPtr : void?) : void>>`。核实：`ast_boost.das:664 table<string; string>` |
| 67-68 | `apply(var func : FunctionPtr, var group ..., args ..., var errors ...)` 参数用**逗号** | 参数分隔用 `;`：`apply(var func : FunctionPtr; var group : ModuleGroup; args : AnnotationArgumentList; var errors : das_string)` |
| 134 | `def DispatchMsg(msgID : uint, sessionID : uint, msgPtr : void?)` 逗号 | `def DispatchMsg(msgID : uint; sessionID : uint; msgPtr : void?)` |
| 多处 | 语句结尾 `;` | gen2 不需要（无害，但与项目 das 风格不一致，建议去掉） |

> 规则：**gen2 里 `def` 参数、`table<>`/`function<>` 类型参数，一律用 `;` 分隔，不是 `,`**。命名参数才用 `[name = value]` 方括号。

### A2. `DasEngine.cpp:237-244` `img.ctx->getAllFiles()` 在 simulate 之前调用 → 空指针崩溃 + 死代码

```cpp
// Compile() 末尾——此时还没 SimulateImage，img.ctx == nullptr
if (!dasbinPath.empty() && img.program && !img.program->failed())
{
    std::vector<std::pair<std::string, int64>> depFiles;
    for (auto *file : img.ctx->getAllFiles())   // ← img.ctx 是 null，崩溃
    {
        depFiles.emplace_back(file->name, img.fileAccess->getFileMtime(file->name));
    }
    // ← 而且 depFiles 收集完根本没调 Save()，是死代码
}
```

两个问题：(1) `img.ctx` 由 `SimulateImage` 创建，`Compile` 阶段还是 null，`getAllFiles()` 直接崩；(2) 收集完没 `DasLangSerializer::Save`，`.dasbin` 永远不生成。

**修**：把「收集依赖 + Save」整段移出 `Compile`，放到 `Load()` 里 `SimulateImage` 成功之后：

```cpp
// Load()，SimulateImage(img) 成功之后、std::move 之前
if (_cfg.mode == EScriptMode::Release && !_cfg.dasbinDir.empty() && dasbinPath.empty())
{
    // 仅当本次是"全量编译"（dasbinPath 为空=没走缓存命中）才写缓存
    std::vector<std::pair<std::string, int64>> deps;
    for (auto *file : img.ctx->getAllFiles())
    {
        if (file && !file->name.empty())
        {
            deps.emplace_back(file->name, img.fileAccess->getFileMtime(file->name));
        }
    }
    std::string outPath = std::format("{}/{}.dasbin", _cfg.dasbinDir, entryFile);
    DasLangSerializer::Save(outPath, img.program, *img.moduleGroup, deps, _cfg.dasbinKeyHex);
}
```

> 注意：命中缓存时 `dasbinPath` 非空且成功——那种情况不该再 Save（会拿反序列化的 program 覆盖）。上面用 `dasbinPath.empty()` 判定"本次是全量编译"。但 `Load` 里 dasbinPath 是局部变量，需要把"是否命中缓存"这个信息从 `Compile` 传出来（加个 `bool &fromCache` 出参，或 `Compile` 返回结构体带标志）。

### A3. F2 未修：`NEED_ALL_DEFAULT_MODULES` 在 `namespace MMO` 内 → 链接错误

`DasEngine.cpp:53` 仍在 namespace 里展开。`NEED_MODULE` 宏生成块作用域 `extern das::Module* register_Module_BuiltIn();`，在 `namespace MMO` 内绑成 `MMO::register_Module_BuiltIn`，而真定义在全局 `::` → MinGW/clang 链接失败 `undefined reference to MMO::register_Module_BuiltIn()`（已实测复现；MSVC 可能容忍）。

**修**（顺序你已对，只需换 namespace 安全形式）：

```cpp
// DasEngine.cpp 顶部，#include 之后、namespace MMO 之前（文件作用域）
DECLARE_ALL_DEFAULT_MODULES;

// Initialize() 内，替换 NEED_ALL_DEFAULT_MODULES;
PULL_ALL_DEFAULT_MODULES;
```

定义见 `daScriptModule.h:72-96`。

---

## 🟡 B. 功能 / 一致性

### B1. `main.das` 没 require handler 模块 → 分发全失效

`Script/World/main.das` 只 `require Common`，没 require `MsgHandlerRegistry`，也没有任何业务 handler 文件。后果：`[msg_handler]` 的 `[init]` 注册代码不进 program，`g_HandlerRegistry` 运行时为空，所有消息分发落空。

**修**：建业务 handler 文件（如 `Script/World/Handlers.das`，含 `[msg_handler] def handle_move(...)`），`main.das` `require Handlers`；`Handlers.das` `require MsgHandlerRegistry public` + `require Common`。链条：`main → Handlers → MsgHandlerRegistry`（把注册 `[init]` 拉进 program）。

### B2. `ScriptDispatchRegistry` 改成了非静态成员，但生成器产静态调用 → 冲突

你把 `Register`/`Dispatch` 从静态改成实例成员（`std::array _msgFuncs` 实例字段）。但 `GenMsgBindings.py` 生成的 `RegisterXxxMsgDispatch()` 是**静态自由函数**，里面 `ScriptDispatchRegistry::Register(msgID, &fn)` 是**静态调用**——与实例成员不兼容，编译不过。

两个选择：
- **（推荐）回到静态单例**：`Register`/`Dispatch`/`Table()` 全 static（我文档 01 的版本）。生成的静态注册函数直接可用，最省事。
- 或：注册表做成引擎持有的实例，生成器改成 `DasLangEngine::GetIns().DispatchRegistry().Register(...)`——改动大，不建议。

`_msgFuncs` 还有个小 bug：`std::array` 实例成员**未初始化**（没 `{}`），元素是野指针。静态版用 `static std::array<...> table{}` 零初始化才安全。

### B3. AOT-4 未修彻底：Log 函数仍是 static，AOT 生成的 cpp 链接不到

`DasCommonModule::aotRequire` emit 了 `#include "...DasCommonModule.h"`，但 `LogInfo/LogWarn/LogError` 是 `.cpp` 里的 `static`、头里无声明 → AOT 生成的 `.cpp` 里 `LogInfo(...)` 调用无法解析（且它们在 `namespace MMO`，AOT 发射的是无限定名）。

**修**（见 04 篇 AOT-4）：
1. 新建 `Src/ScriptEngine/Module/DasCommonBinds.h`，非 static 声明三个 Log 函数。
2. `.cpp` 去掉 `static`、`#include "DasCommonBinds.h"`。
3. 绑定 cppName 用**全限定** `"MMO::LogInfo"`（AOT 发射 `MMO::LogInfo(...)` 才能解析）。
4. `aotRequire` emit `#include "ScriptEngine/Module/DasCommonBinds.h"`（不是 DasCommonModule.h）。

### B4. `RebindFunctions` 找 `"DispatchMsg"` —— 与 das 导出一致 ✓（已修好）

das 侧 `[export] def DispatchMsg`，C++ 侧 `findFunction("DispatchMsg")`——一致。注意这与我文档 01 建议的 `dispatch_msg` 不同，你统一用了 `DispatchMsg`，**自洽即可**，无需改。`Init`/`Update` 同样一致。

---

## 🟢 C. 清理

- **C1**：`DasCommonModule.h` 死成员 `_host`/`_timingWheel`（没赋值没读）——删，连带去掉 `Common/Timer/TimingWheel.h`、`IDasHost.h` 的 include。
- **C2**：`DasEngineConfig.mainFile` 有字段但 `Load` 用的是传入 `entryFile` 参数，没读 `cfg.mainFile`——要么 `Load()` 默认用 `cfg.mainFile`，要么删字段。
- **C3**：`Update()` 现在零参，`Tick` 传 1 个 arg——eval 不校验 arity，当前无害；`Update` 以后要 `(sceneID, dt)` 时记得同步 `Tick` 的 args。
- **C4**：`ReadDasbinHeader` 后又 `memcmp(header.magic, "DASBIN", sizeof(header.magic))`——`sizeof(header.magic)` 是 7（含 `\0`），而字面量 `"DASBIN"` 是 7 字节（6 char + `\0`），恰好对上，但**可读性差**，建议直接比 `kDasbinMagic`。
- **C5**：`WorldDasModule.h` 只有声明没有 `.cpp` 实现——记得补 `WorldDasModule.cpp`（`CreateModules` 里建 WorldModule + `RegisterAllProtoMessageTypes`；`OnContextSwapped`/`DrainTimers` 等）。

---

## 修复优先级（建议顺序）

1. **A1**（das 语法）——否则脚本层根本编译不过。
2. **A3**（DECLARE/PULL）——否则 C++ 链接不过。
3. **B2**（注册表静态化）——否则生成的 gen.cpp 编译不过。
4. **A2**（Save 移位置 + 修 null 崩溃）——否则 Release 启动即崩 / 无缓存。
5. **B1**（main require handler）——否则跑起来但分发失效。
6. **B3**（AOT-4 Log 移公共头）——AOT 落地前必修，见 AOT 报告。
7. **C 系列**清理。

前 5 项修完，Develop 模式应能编译 + 启动 + 跑通最小 `main.das` + 消息分发。
