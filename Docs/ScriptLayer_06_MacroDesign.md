# ScriptLayer_06 — 脚本宏系统设计规范

> 状态：设计定稿（经 workflow 对抗验证，3 技术假设 confirmed，5 major 缺陷已修正）
> 关联：[[ScriptLayer_01_MsgBinding]]（宏注册机制）· [[ScriptLayer_04_AOT]]（AOT 批次）· [[ScriptLayer_05_GenBindings_Fixes]]（生成器）
> 实现：见 `ScriptLayer_07_MacroImplement.md`

## 1. 设计目标（排序）

1. **正确性**：编译期捕获一切可捕获错误（类型不符、ID 冲突、命名漂移），不依赖运行期
2. **单一真相源**：命名约定只存在一处，其他全部生成/派生
3. **AOT/热重载兼容**：宏产物稳定、可 AOT、可跨重载重建
4. **使用体验**：声明式语法不变，错误信息精确到「你的 handler 哪里写错了」

## 2. 现状与问题（审查结论）

| # | 问题 | 位置 | 严重度 |
|---|---|---|---|
| P1 | 命名约定 **4 处手写**（Python `camel_to_msg_id`/`event_msg_to_enum` + das `MsgNameToMsgID`/`EventTypeNameToEnum`），靠「当前实测等价」维系 | `GenMsgBindings.py:128,950`、`MsgHandlerRegistry.das:17`、`GameEventRegistry.das:49,79` | 高——改约定要改 4 处 |
| P2 | 双 handler 注册同一消息 ID → **运行期静默覆盖**，最难排查 | 宏注入直接 `g_HandlerRegistry[id] = ...` | 高 |
| P3 | `[game_system]` interval 类型不符 → **静默按 0（每帧）** | `GameSystemRegistry.das:48-53` | 中 |
| P4 | handler 包装是匿名 lambda（`_localfunction_<mod>_<line>_<counter>`），调试/AOT 名不可读 | 三宏 `qmacro_expr` 注入 `@@(...)` | 中 |
| P5 | 校验全在 apply()（parse 期），「类型已绑定/可 dispatch」语义校验会假阴性 | 三宏 apply | 低 |
| P6 | AOT 覆盖依赖「handler 恰好进 extra_aot 批次」，新文件会**静默解释执行** | `Src/World/xmake.lua:14-20` | 高（潜伏） |
| P7 | 三文件各写一份 `IsUpper`/`ToUpperChar` 算法 | 三个 Registry.das | 中 |
| P8 | `require daslib.ast` 点号 vs 官方 `daslib/ast` 斜杠混用 | 三个 Registry.das | 低 |

## 3. 架构：四层

```
┌─ 使用层（业务代码，声明式不变）───────────────────────┐
│  [msg_handler]  def handle_move(sessionID : uint; req : MoveReq)  │
│  [game_event]   def EntityDamaged(ev : EntityDamagedEvent)        │
│  [game_system(interval = 1.0)] def ai_tick(dt : float)            │
└──────────────────────────────────────────────────────────┘
        │ apply() 编译期（结构化校验 + 查表 + 生成）
        ▼
┌─ MacroCommon.das（三宏共享助手，def public）───────────┐
│  GetHandleArgType / CheckSig / EmitRegistration /      │
│  RegID / 错误模板                                      │
└──────────────────────────────────────────────────────────┘
        │ 查映射（compiling_module() 动态取模块）
        ▼
┌─ 数据层（生成器产物，单一真相源）─────────────────────┐
│  GenMsgBindings.py → TypeNameRegistry.gen.cpp         │
│   MsgTypeToID(name):int64   —— MoveReq → MSG_* 值      │
│   EventTypeToID(name):int64 —— EntityDamaged → 事件值  │
│   （同一份 proto 产出；-1 = 未注册）                    │
└──────────────────────────────────────────────────────────┘
        ▼
┌─ 运行层（手写 [export] 分发，宏只产注册条目）─────────┐
│  DispatchMsg / DispatchGameEvent / TickGameSystems     │
└──────────────────────────────────────────────────────────┘
```

### 3.1 数据层：命名约定收敛为生成产物（核心改动）

**机制**：`GenMsgBindings.py` 已解析全部 proto 消息名与枚举值，让它**同时生成 C++ 查表函数**，绑定进 Common 模块，宏 apply 直接查。

```cpp
// TypeNameRegistry.gen.cpp（生成器产物，Src/ScriptEngine/Module/）
#include "ScriptEngine/Module/TypeNameRegistry.gen.h"
namespace MMO::Script
{
    // MoveReq → EMsgID 值；未注册返回 -1（保留哨兵，消息 ID 必须为正 int32）
    int64_t MsgTypeToID(const char* typeName)
    {
        static const std::unordered_map<std::string, int64_t> table = {
            { "MoveReq", static_cast<int64_t>(MMO::Proto::MSG_MOVE_REQ) },
            { "LoginAuthReq", ... },
            // ... 全部 *Req 由生成器产出
        };
        auto it = table.find(nullptr != typeName ? typeName : "");
        return it == table.end() ? -1 : it->second;
    }

    // EntityDamagedEvent → EGameEventType 值；未注册返回 -1
    int64_t EventTypeToID(const char* typeName)
    {
        // 事件枚举值来自 GameEvent.proto 的 enum EGameEventType
        static const std::unordered_map<std::string, int64_t> table = { ... };
        ...
    }
}
```

**绑定**（`DasCommonModule.cpp`，直接 addExtern，**无需 [macro_function]**）：

```cpp
#include "ScriptEngine/Module/TypeNameRegistry.gen.h"
// 构造器内：
das::addExtern<DAS_BIND_FUN(MMO::Script::MsgTypeToID)>(
    *this, lib, "MsgTypeToID", das::SideEffects::none, "MMO::Script::MsgTypeToID")
    ->args({"typeName"});
das::addExtern<DAS_BIND_FUN(MMO::Script::EventTypeToID)>(
    *this, lib, "EventTypeToID", das::SideEffects::none, "MMO::Script::EventTypeToID")
    ->args({"typeName"});
```

**为什么优雅**：
- 命名规则**物理上只剩生成器一处**，das 侧零规则
- 枚举成员存在性检查 + ID 查重**合并进查表**：`-1` 即「类型名拼错或 proto 未定义」
- 类型→ID 关系由生成器从同一份 proto 产出，**永不漂移**
- 新增消息类型 = 重新生成 = 宏自动认识

> ⚠️ **技术验证（confirmed）**：`[macro_function]` 只对 das 侧函数有意义（`module_builtin_runtime.cpp:75-81` 仅置 `macroFunction` 位），**不能标注 C++ addExtern**。但 C++ addExtern 是 builtin，天然对宏 context 可见——宏 apply() 里直接调用即可，机制与现有 `find_compiling_module`/`module_find_enumeration`（同为 C++ extern）一致。

### 3.2 工具层：MacroCommon.das

把三个宏重复的算法与注册模板抽成一个模块，三宏只做薄封装。

```das
module MacroCommon public
options gen2
options indenting = 4
options no_global_variables = false   // 宏期查重表需要全局变量

require daslib.ast
require daslib.ast_boost
require daslib.templates_boost

// ── 编译期查重表（三键空间，只被宏期代码引用）──
// ⚠️ 仅宏期读写；若被运行期函数引用会进 AOT surface，remove_unused_symbols 才裁剪
var private g_registered : table<string, string>   // "msg:ID"/"evt:ID"/"sys:name" → handler 名

// 注册 ID 并查重；重复返回 false（调用方报编译错）
def public RegisterID(namespace : string; id : int64; handlerName : string) : bool
{
    let key = namespace + ":" + int64_to_string(id)
    if (key_exists(g_registered, key))
    {
        return false
    }
    g_registered[key] = handlerName
    return true
}

// 取 handler 函数第 idx 个参数的类型并校验是 handle
// 返回类型名（含 annotation），非 handle 返回 null
def public GetHandleArgType(func : FunctionPtr; idx : int) : string?
{
    if (length(func.arguments) <= idx) { return null }
    let t = func.arguments[idx]._type
    if (t.baseType != Type.tHandle || t.annotation == null) { return null }
    return string(t.annotation.name)
}

// 校验 handler 签名骨架（参数个数 + 首参类型），返回精确错误文案
def public CheckFirstArgIsUInt(func : FunctionPtr; errors : das_string) : bool { ... }

// 生成具名包装函数并加入当前模块，返回包装函数名
def public EmitWrapper(func : FunctionPtr; handledType : string;
                       wrapperName : string; sigDoc : string) : string
{
    // 见实现文档 §4.3：qmacro_function 生成 def private `wrapperName`(arg0..; msgPtr : void?)
    // 内部 unsafe(reinterpret<handledType?> msgPtr) + _::调用原函数
    ...
}

// 把「注册条目」注入 init 块
def public EmitRegistration(regTable : string; keyExpr : ExpressionPtr;
                            wrapperName : string; at : LineInfo) : bool { ... }
```

**关键约束**：
- 助手全部 **`def public`**——三个注册宏是独立 module，`def private` 不跨模块
- 目标模块一律 `compiling_module()` 动态取，**不硬编码**——宏跑在哪个模块就写哪个模块
- 查重表用 `var private` + `options no_global_variables = false`（linq_fold 同款范式，`linq_fold_common.das:641`）

### 3.3 宏本体：具名包装 + 编译期查重 + 两段式校验

**3.3a 具名包装函数替代匿名 lambda**（验证 #3 confirmed，动机修正为可读性/查重）

```das
[function_macro(name="msg_handler")]
class MsgHandlerAnnotation : AstFunctionAnnotation
{
    def override apply(var func : FunctionPtr, var group : ModuleGroup,
                       args : AnnotationArgumentList, var errors : das_string) : bool
    {
        // ── 结构化校验（parse 期可用）──
        if (length(func.arguments) < 2) { errors := "[msg_handler] 至少两个参数"; return false }
        if (func.arguments[0]._type.baseType != Type.tUInt) { errors := "[msg_handler] 第一参须 uint"; return false }
        let typeName = MacroCommon::GetHandleArgType(func, 1)
        if (typeName == null) { errors := "[msg_handler] 第二参须消息类型（如 req : MoveReq）"; return false }

        // ── 查表（单一真相源）：-1 = 类型名拼错或 proto 未定义 ──
        let id = MsgTypeToID(typeName)
        if (id < 0) { errors := "[msg_handler] 类型 {typeName} 不在 MsgID.proto——检查命名或重新生成绑定"; return false }

        // ── 编译期查重：同一 ID 二次注册 → 编译报错 ──
        if (!MacroCommon::RegisterID("msg", id, string(func.name)))
        {
            errors := "[msg_handler] 消息 {typeName} 已被重复注册——一个消息只能有一个脚本 handler"
            return false
        }

        // ── 生成具名包装 + init 注册（固定 void? 签名，apply 期可行）──
        let wrapperName = MacroCommon::EmitWrapper(func, typeName, "reg`" + string(func.name), ...)
        MacroCommon::EmitRegistration("g_HandlerRegistry", ...)
        func.flags.privateFunction = true
        return true
    }

    // ── 语义校验（infer 后）：类型确实已绑定为 handle 且可 dispatch ──
    def override finish(var fn : FunctionPtr, var group : ModuleGroup, ...) : bool
    {
        // 依赖完整类型的检查放这里（apply 在 parse 期可能对别名/未解析类型假阴性）
        return true
    }
}
```

**3.3b 两段式校验（修正评审 #4）**

| 阶段 | 时机 | 可做校验 |
|---|---|---|
| `apply()` | PARSE 期（`parser_impl.cpp:211-248`） | 参数个数、`baseType`、`annotation != null`、`find_arg` 参数类型 |
| `finish()` | INFER 后（`ast_parse.cpp:967` finalizeAnnotations） | 类型已绑定为 handle、枚举已注册、可 dispatch |

**3.3c 哨兵 -1 显式检查（修正评审 #5）**

宏侧必须先 `if (id < 0)` 报编译错，再 `uint(id)`。**严禁**把 -1 直接 `uint(...)`——会注册 `0xFFFFFFFFFFFFFFFF` 截断的巨值假键。

### 3.4 运行层：保持手写集中分发

`DispatchMsg` / `DispatchGameEvent` / `TickGameSystems` 保持 `[export]` 手写集中，宏只产注册条目。

**三个分发函数签名是 C++ 契约**，冻结：
- `DispatchMsg(msgID:uint, sessionID:uint, msgPtr:void?):bool` —— C++ 侧 `CallScriptFunctionInR<bool>` 校验 arity
- `DispatchGameEvent(evType:uint16, payload:void?)` —— `WorldServer.cpp:306`
- `TickGameSystems(dt:float, now:float)` —— `WorldServer.cpp:336`

改动即破坏生成代码/arity 断言（响亮失败，可接受）。宏侧只允许向注册表写条目。

## 4. AOT 覆盖约束（最重要的落地约束，修正评审 #2）

### 4.1 问题

`setup_call_list` 把 init 块加进 **handler 所在模块**（`compiling_module()`，`ast_boost.das:296-331`）。`collectUsedFunctions` 只收集 used 函数（`aot_cpp.das:4077-4091`，按模块 each）。

当前全部 handler 在 `Script/World/main.das`，AOT 批次含 `main.das` + 三个注册文件（`Src/World/xmake.lua:14-20` extra_aot），所以 100% 覆盖。**但未来新增的 handler 文件若不在 AOT 批次，注册代码会静默解释执行**——`fail_on_no_aot=false` 下无任何报错。

### 4.2 约束（文档 + 宏报错信息显式化）

> **硬约束**：handler 所在文件必须是 AOT 批次的入口（`World/main.das`）或 extra_aot 成员。

宏报错信息在「未找到 Common 模块」旁同样提示：「若本文件是新 handler 文件，需加入 xmake das_aot 的 extra_aot 清单」。

### 4.3 长期修复：xmake rule 自动扫描

`das_aot` rule 里自动把含 `[msg_handler]/[game_event]/[game_system]` 注解的 `.das` 文件加入批次：

```lua
-- Rules.das_aot 的 before_build 里：
-- 扫描 Script/**/*.das，grep 到注解则视为 handler 文件，加入 AOT 文件清单
for _, f in ipairs(os.files(path.join(scriptRoot, "**/*.das"))) do
    local content = io.open(f, "r"):read("*a")
    if content:find("%[msg_handler%]") or content:find("%[game_event%]")
       or content:find("%[game_system%]") then
        table.insert(aotBatch, f)   -- 相对 scriptRoot 的路径
    end
end
```

## 5. 修复清单汇总

| # | 现状 | 修复 | 文档节 |
|---|---|---|---|
| P1 | 4 处手写命名规则 | 生成器产出 2 个 C++ 查表函数 | §3.1 |
| P2 | 运行期静默覆盖 | 编译期查重表报错 | §3.2/§3.3a |
| P3 | interval 类型不符静默 0 | apply() 显式校验 + 报编译错 | §3.3b |
| P4 | 匿名 lambda | 具名 `\`reg\`name` 包装 | §3.3a |
| P5 | 全在 apply() | apply() 结构化 + finish() 语义两段 | §3.3b |
| P6 | 依赖「恰好在批次」 | 约束显式化 + rule 自动扫描 | §4 |
| P7 | 三份算法拷贝 | MacroCommon.das 单一实现 | §3.2 |
| P8 | require 点号/斜杠混用 | 统一 `daslib/xxx` 斜杠 | §3.2 |

## 6. 落地顺序

1. **§3.1 数据层**：GenMsgBindings.py 产出 TypeNameRegistry.gen.{h,cpp} + DasCommonModule 绑定 + 冒烟探针（宏 apply 里调 MsgTypeToID）
2. **§3.2 工具层**：MacroCommon.das
3. **§3.3 宏本体**：三宏逐个改造（先 msg_handler，验证后复制范式）
4. **§3.4 运行层**：不改（验证签名契约）
5. **§4 AOT**：约束文档化 + xmake rule 自动扫描
6. **验证**：debug 构建 + 热重载 + Release AOT 覆盖率 100% + 负向用例（重复注册报错 / interval=1 整型报错 / -1 哨兵）

## 7. 验证要点（负向用例）

- 同一 `MSG_MOVE_REQ` 两个 handler → 编译报「已被 Xxx 注册」
- `[game_system(interval = 1)]`（整型）→ 编译报「须为 float 字面量，写 1.0」
- `[msg_handler] def f(sessionID:uint; req: NotInProto)` → 编译报「类型不在 MsgID.proto」
- `MsgTypeToID("Unknown")` → -1，宏侧报错（哨兵检查生效）
- Release AOT 覆盖率保持 100%（含新具名 wrapper `reg` 前缀符号）
