# ScriptLayer_07 — 宏系统落地实现

> 前置：`ScriptLayer_06_MacroDesign.md`（设计规范，含技术验证结论）
> 目标：按 §6 落地顺序逐条实现，**每步可照抄、能编译、可验证**
> 铁律：每完成一步跑构建验证，再进下一步。宏代码先 `msg_handler` 改造验证，再复制范式到另两个。

---

## ✅ 已落地（2026-08-06）

- **Step 0（冒烟探针）**：完成。查表函数绑定 + 运行期调用验证全绿
- **Step 1（MacroCommon.das）**：完成。查重表（RegisterID/RegisterSys）+ 校验助手（CheckArgCount/CheckFirstArgIsUInt/CheckFirstArgIsFloat/GetHandleArgType）+ EmitRegistration 模板
- **Step 2（msg_handler 改造）**：完成。查表 + 查重 + 哨兵 + 两段式校验骨架
- **Step 4（game_event + game_system 改造）**：完成。game_event 用 EventTypeToID + evt 键空间查重；game_system 修 interval 整型报错 + sys 键空间查重
- **Step 5（AOT 覆盖自动扫描）**：完成。`Rules.das_aot` 的 on_config + before_build 内联扫描 `Script/**/*.das`，行首命中 `[msg_handler]/[game_event]/[game_system]` 即并入 AOT 批次（与 entry + extra_aot 去重）。验证：临时 handler 文件自动进批次（`World_handlers_test.das.cpp` 生成）；未 require 宏模块的 handler 文件响亮报错（非静默解释执行）；注释里的注解不误报（MacroCommon 不误入批次）

**Step 5 踩坑**：
1. **xmake rule 闭包看不到文件级函数**（沙箱隔离）——扫描逻辑须**内联**到 on_config/before_build 两处，不能用文件级 helper
2. **`content:match("^[ \t]*%[x]")` 的 `^` 锚定文件头不跨行**——须逐行扫 `for line in string.gmatch(content, "[^\r\n]+")`
3. **注释里的 `[msg_handler]` 会误报**（如 MacroCommon.das 头部注释）——行首匹配 `^[ \t]*%[msg_handler%]` 排除注释（注释里注解前面有 `//` 非行首）

**关键踩坑（实现时实测）**：
1. **`string(...)` 返回 const**：`string(t.annotation.name)` 产生 `string const`，赋给 `string?`/`string` 都报 `error[30343]`。解法：`var s = string(...); return s`（var 声明脱离 const）返回非 const `string`
2. **跨模块 out 参数失效**：`def GetHandleArgType(func, idx, var out) : bool` 跨模块调用时 `out` 写回失效（helper='' 但内联='MoveReq'）。解法：改用**返回值 + 空串表示失败**（类型名不可能为空）
3. **跨模块调用 `模块名::函数` 与裸名均可**：daslang 支持 `MacroCommon::CheckArgCount(func, 2)` 限定调用
4. **`RegisterID` 的 int64 id 不接受 string**：game_system 用 string 键，需单独 `RegisterSys(nameSpace, name)`
5. **宏 apply 在宏定义模块的 macroContext 跑**：`[msg_handler]`（MsgHandlerRegistry 只 require Common）能看到 Common 的 `MsgTypeToID`；`[game_event]`（GameEventRegistry require world）能看到 world 的 `EventTypeToID`。**查表函数必须绑在对应宏可见的模块**（MsgType→Common，EventType→world）
6. **AotGen 是验证宏的正确工具**：`Bin/windows-x64-release/AotGen.exe Script <entry.das> <out.cpp>`，链接全部 native 模块 + 宏支持；`daslang.exe` 工具看不到 C++ native 模块（`require Common` 失败），无法验证

**验证结果**：
- Release 构建通过，`AOT coverage 21/21 (100%)`
- `RebindFunctions Init=ok Update=ok DispatchMsg=ok`
- `game_system: ai_tick` 每秒错峰稳定运行
- 负向：重复消息/事件/系统 → 编译报错；interval 整型 → 编译报错；未注册类型 → 哨兵报错

## Step 0 — 冒烟探针（先验证核心假设，再动刀）

**目的**：验证「宏 apply() 里直接调用 C++ addExtern 查表函数」在本项目真实可用。

### 0.1 加查表函数到 DasCommonModule

**新增头文件** `Src/ScriptEngine/Module/TypeNameRegistry.gen.h`：

```cpp
#pragma once
#include <cstdint>
namespace MMO::Script
{
    // 消息类型名 → EMsgID 值；未注册返回 -1（哨兵，消息 ID 必须为正 int32）
    int64_t MsgTypeToID(const char* typeName);
    // 事件类型名 → EGameEventType 值；未注册返回 -1
    int64_t EventTypeToID(const char* typeName);
}
```

**新增实现** `Src/ScriptEngine/Module/TypeNameRegistry.gen.cpp`：

```cpp
#include "ScriptEngine/Module/TypeNameRegistry.gen.h"
#include "Proto/AutoGen/MsgID.pb.h"          // EMsgID 枚举
#include "Proto/AutoGen/GameEvent.pb.h"      // EGameEventType 枚举
#include <unordered_map>
#include <string>

namespace MMO::Script
{
    int64_t MsgTypeToID(const char* typeName)
    {
        static const std::unordered_map<std::string, int64_t> table = {
            { "MoveReq",         static_cast<int64_t>(MMO::Proto::MSG_MOVE_REQ) },
            { "LoginAuthReq",    static_cast<int64_t>(MMO::Proto::MSG_LOGIN_AUTH_REQ) },
            { "LoginEnterWorldReq", static_cast<int64_t>(MMO::Proto::MSG_LOGIN_ENTER_WORLD_REQ) },
            { "HeartbeatReq",    static_cast<int64_t>(MMO::Proto::MSG_HEARTBEAT_REQ) },
            // ⚠️ 生成器自动维护：新增 proto 消息后重新运行 GenMsgBindings.py
        };
        auto it = table.find(nullptr != typeName ? typeName : "");
        return it == table.end() ? -1 : it->second;
    }

    int64_t EventTypeToID(const char* typeName)
    {
        static const std::unordered_map<std::string, int64_t> table = {
            { "EntityEnterAOIEvent", static_cast<int64_t>(MMO::Proto::GAME_EVENT_ENTITY_ENTER_AOI) },
            { "EntityDamagedEvent",  static_cast<int64_t>(MMO::Proto::GAME_EVENT_ENTITY_DAMAGED) },
            // ⚠️ 生成器自动维护
        };
        auto it = table.find(nullptr != typeName ? typeName : "");
        return it == table.end() ? -1 : it->second;
    }
}
```

### 0.2 绑定进 DasCommonModule

`Src/ScriptEngine/Module/DasCommonModule.cpp`：

```cpp
#include "ScriptEngine/Module/TypeNameRegistry.gen.h"
// ... 构造器内，Log 绑定之后：
das::addExtern<DAS_BIND_FUN(MMO::Script::MsgTypeToID)>(
    *this, lib, "MsgTypeToID", das::SideEffects::none, "MMO::Script::MsgTypeToID")
    ->args({"typeName"});
das::addExtern<DAS_BIND_FUN(MMO::Script::EventTypeToID)>(
    *this, lib, "EventTypeToID", das::SideEffects::none, "MMO::Script::EventTypeToID")
    ->args({"typeName"});
```

⚠️ `addExtern` 的 cppName 用**全限定** `"MMO::Script::MsgTypeToID"`（AOT 发射需要，见 ScriptLayer_04）。

### 0.3 生成器接入

`Tools/Script/GenMsgBindings.py` 增加 emit 函数，产出上述 `TypeNameRegistry.gen.{h,cpp}`。映射数据来自已解析的：
- `msg_by_name`（消息名 → MessageInfo，含 `msg_id` 枚举值）
- `event_msgs`（事件消息名列表）+ `event_enum_values`（EGameEventType 真实值）

生成逻辑与现有 `generate_game_event_bindings_*` 同构（`GenMsgBindings.py:998+`），在 `main` 流程末尾调用。

### 0.4 冒烟探针（临时验证）

在 `Script/World/main.das` 的 `Init()` 里临时加：

```das
[export]
def Init()
{
    // 冒烟：查表函数在宏 context 与运行期都可见
    let id = MsgTypeToID("MoveReq")
    let bad = MsgTypeToID("NotInProto")
    LogInfo("probe: MoveReq={id} unknown={bad}")   // 期望 4 -1（值取决于 proto）
}
```

**验证**：debug 构建 + 运行 WorldServer，日志出现 `probe: MoveReq=<正数> unknown=-1`。通过后删除探针。

---

## Step 1 — MacroCommon.das（共享工具层）

**新增** `Script/Common/MacroCommon.das`：

```das
// Script/Common/MacroCommon.das
//
// 三宏（[msg_handler]/[game_event]/[game_system]）共享助手。
// 单一真相源：命名规则在生成器（TypeNameRegistry.gen.cpp），这里只查表。
// 查重表是编译期状态（宏 context），不会进运行期——linq_fold 同款范式。

module MacroCommon public
options gen2
options indenting = 4
options no_global_variables = false    // 宏期查重表需要

require daslib.ast
require daslib.ast_boost
require daslib.templates_boost
require daslib.strings_boost

// ── 编译期查重表 ──
// 键："msg:<ID>" / "evt:<ID>" / "sys:<name>" → handler 名
// ⚠️ 仅宏期读写；禁止运行期引用（否则进 AOT surface）
var private g_registered : table<string, string>

def public RegisterID(namespace : string; id : int64; handlerName : string) : bool
{
    let key = "{namespace}:{id}"
    if (key_exists(g_registered, key))
    {
        return false
    }
    g_registered[key] = handlerName
    return true
}

// 取 func 第 idx 参类型；校验是 handle（tHandle 且 annotation 非空），返回类型名或 null
def public GetHandleArgType(func : FunctionPtr; idx : int) : string?
{
    if (length(func.arguments) <= idx) { return null }
    let t = func.arguments[idx]._type
    if (t.baseType != Type.tHandle || t.annotation == null) { return null }
    return string(t.annotation.name)
}

// 校验 handler 首参是 uint
def public CheckFirstArgIsUInt(func : FunctionPtr) : bool
{
    if (length(func.arguments) < 1) { return false }
    let t = func.arguments[0]._type
    return t.baseType == Type.tUInt && t.fixedDim == 0
}

// 校验 handler 首参是 float（game_system 用）
def public CheckFirstArgIsFloat(func : FunctionPtr) : bool
{
    if (length(func.arguments) < 1) { return false }
    return func.arguments[0]._type.baseType == Type.tFloat
}

// 生成具名包装函数（def private `reg`<name>）并加入当前模块。
// wrapper 边界签名固定：arg0..argN + msgPtr : void?；内部 reinterpret + 同模块调用原函数。
// 返回 wrapper 函数名；失败返回 null。
def public EmitWrapper(func : FunctionPtr; handledType : string;
                       wrapperName : string; callSig : string) : string?
{
    var wrapper <- qmacro_function(wrapperName) $($a(wrapperArgs)) : void {
        let typed = unsafe(reinterpret<$t(handledType)?> msgPtr)
        $c("_::{func.name}")(typed, msgPtr)
    }
    wrapper.flags.privateFunction = true
    if (!(compiling_module() |> add_function(wrapper)))
    {
        return null
    }
    return wrapperName
}
```

> ⚠️ `qmacro_function` 生成 `def private` 的可靠写法见 Step 2 —— 这一步先不落地函数体，只建模块骨架 + 查重表 + 简单校验助手，跑通构建后再补 EmitWrapper。

---

## Step 2 — 改造 `[msg_handler]`（先做一个，验证后复制范式）

`Script/Common/MsgHandlerRegistry.das` 改造：

```das
require daslib/ast
require daslib/ast_boost
require daslib/macro_boost
require daslib/strings_boost
require daslib/templates_boost   // $c 模板名解析需要

require Common
require MacroCommon

[function_macro(name="msg_handler")]
class MsgHandlerAnnotation : AstFunctionAnnotation
{
    def override apply(var func : FunctionPtr, var group : ModuleGroup,
                       args : AnnotationArgumentList, var errors : das_string) : bool
    {
        // ── 结构化校验（parse 期可用）──
        if (length(func.arguments) < 2) {
            errors := "[msg_handler] 函数至少需要两个参数: (sessionID : uint; req : <消息类型>)"
            return false
        }
        if (!MacroCommon::CheckFirstArgIsUInt(func)) {
            errors := "[msg_handler] 第一个参数必须是 sessionID : uint"
            return false
        }
        let typeName = MacroCommon::GetHandleArgType(func, 1)
        if (typeName == null) {
            errors := "[msg_handler] 第二个参数必须是消息类型（如 req : MoveReq），实际不是 handled type"
            return false
        }

        // ── 查表（单一真相源）：-1 = 类型名拼错或 proto 未定义 ──
        let id = MsgTypeToID(typeName)
        if (id < 0) {
            errors := "[msg_handler] 消息类型 {typeName} 不在 MsgID.proto——检查命名或重新生成绑定"
            return false
        }
        let msgID = uint(id)   // ⚠️ 只有 id >= 0 才到这里，哨兵已拦截

        // ── 编译期查重：同一 ID 二次注册 → 编译报错 ──
        if (!MacroCommon::RegisterID("msg", id, string(func.name)))
        {
            errors := "[msg_handler] 消息 {typeName} 已被另一个 handler 注册——一个消息只能有一个脚本 handler"
            return false
        }

        // ── 生成具名包装 + init 注册 ──
        let wrapperName = "reg`" + string(func.name)
        var initBlk <- setup_call_list("msg_handler`init", func.at, true, true)
        initBlk.list |> push(qmacro_expr(${
            g_HandlerRegistry[$v(msgID)] = @@(sessionID : uint; msgPtr : void?) {
                let typedMsg = unsafe(reinterpret<$t(typeName)?> msgPtr)
                $c("_::{func.name}")(sessionID, *typedMsg)
            }
        }))
        func.flags.privateFunction = true
        return true
    }

    def override finish(var fn : FunctionPtr, var group : ModuleGroup,
                        args, progArgs : AnnotationArgumentList, var errors : das_string) : bool
    {
        // 语义校验（infer 后）：需要完整类型的检查放这里
        return true
    }
}

[export]
def DispatchMsg(msgID : uint, sessionID : uint, msgPtr : void?) : bool
{
    var handled = false
    g_HandlerRegistry |> get(msgID)$(handler)
    {
        invoke(handler, sessionID, msgPtr)
        handled = true
    }
    return handled
}
```

**本步要点**：
- `MsgNameToMsgID`/`IsUpper`/`ToUpperChar` 三个手写算法函数**删除**（查表替代）
- `require daslib/xxx` 斜杠统一
- 查重 + 哨兵检查新增
- wrapper 先保留匿名 lambda 形态（**Step 3 再换具名**）——分两步走，每步验证

**验证**：debug 构建 + WorldServer 启动日志 `RebindFunctions ... DispatchMsg=ok` + 运行一次 Move 消息命中 handler。

---

## Step 3 — 具名包装函数（替换匿名 lambda）

**目标**：`handle_move_test` 的注册条目从匿名 lambda 换成具名 `\`reg\`handle_move_test`。

### 3.1 生成具名 wrapper

apply() 里：

```das
// wrapper 签名固定：与 handler 第一参同（sessionID:uint）+ msgPtr:void?
// qmacro_function 生成 def private `reg`<name>(sessionID : uint; msgPtr : void?) : void
let wrapperName = "reg`" + string(func.name)
var wrapper <- qmacro_function(wrapperName) $($a(wrapperArgs)) : void {
    let typed = unsafe(reinterpret<$t(typeName)?> msgPtr)
    $c("_::{func.name}")(sessionID, *typedMsg)
}
wrapper.flags.privateFunction = true
if (!(compiling_module() |> add_function(wrapper)))
{
    errors := "[msg_handler] 无法生成包装函数 {wrapperName}"
    return false
}
```

`wrapperArgs` 构造：`[ new Variable(name:="sessionID", type=clone_type(func.arguments[0]._type), at=func.at), new Variable(name:="msgPtr", type=tVoid? ...) ]`。

### 3.2 init 块注册具名 wrapper（引用而非重新生成）

```das
var initBlk <- setup_call_list("msg_handler`init", func.at, true, true)
// 引用 wrapper 函数地址（ExprAddr），非内联 lambda
initBlk.list |> push(qmacro_expr(${
    g_HandlerRegistry[$v(msgID)] = @@(sessionID : uint; msgPtr : void?) {
        invoke($e(wrapperRef), sessionID, msgPtr)
    }
}))
```

> 简化为直接注册 `@$i(wrapperName)`（ExprAddr），因为 wrapper 签名已是 `(sessionID:uint; msgPtr:void?)`，与 `g_HandlerRegistry` 值类型 `function<(sessionID:uint; msgPtr:void?):void>` 完全匹配：

```das
initBlk.list |> push(qmacro_expr(${
    g_HandlerRegistry[$v(msgID)] = @$i(wrapperName)
}))
```

**验证**：
- debug 构建 + 热重载
- Release AOT 输出中看到 `_Func`reg`handle_move_test_<hash>` 稳定符号（替代 `_localfunction_thismodule_125_1`）
- AOT 覆盖率保持 100%

---

## Step 4 — 复制范式到 `[game_event]` 与 `[game_system]`

### 4.1 `[game_event]`

```das
def override apply(...)
{
    // 结构化校验：至少 1 参 + handle
    if (length(func.arguments) < 1) { errors := "[game_event] 需要至少一个参数: (ev : <事件类型>)"; return false }
    let typeName = MacroCommon::GetHandleArgType(func, 0)
    if (typeName == null) { errors := "[game_event] 参数必须是事件类型（如 ev : EntityDamagedEvent）"; return false }

    // 查表（事件专属）：EntityDamagedEvent → 事件值；-1 未注册
    let id = EventTypeToID(typeName)
    if (id < 0) {
        errors := "[game_event] 事件类型 {typeName} 不在 GameEvent.proto——检查命名或重新生成绑定"
        return false
    }
    let evID = uint16(id)

    // 查重
    if (!MacroCommon::RegisterID("evt", id, string(func.name)))
    {
        errors := "[game_event] 事件 {typeName} 已被另一个 handler 注册——一个事件只能有一个脚本 handler"
        return false
    }

    // 具名 wrapper + init 注册（范式同 Step 3）
    ...
    return true
}
```

**删除** `EventNameToSnake`/`EventTypeNameToEnum`/`IsUpper`/`ToUpperChar`（查表替代）。

### 4.2 `[game_system]`

```das
def override apply(...)
{
    // interval 类型严格校验（修复 P3）：
    //   - tFloat 接受（1.0）
    //   - tInt 报编译错（提示写 1.0）
    //   - 缺失/0 保持每帧语义
    var interval = 0.0
    let arg = find_arg(args, "interval")
    if (arg is tInt)
    {
        errors := "[game_system] interval 必须是 float 字面量（如 1.0），写 {arg} 会被当作每帧——请改为 {arg}.0"
        return false
    }
    elif (arg is tFloat)
    {
        interval = arg as tFloat
    }

    // 签名校验：首参 float
    if (!MacroCommon::CheckFirstArgIsFloat(func))
    {
        errors := "[game_system] 需要参数: (dt : float)"
        return false
    }

    // 用 func.name 字符串做键（无需命名转换，天然无漂移）
    let name = string(func.name)
    if (!MacroCommon::RegisterID("sys", name, name))   // 系统名查重
    {
        errors := "[game_system] 系统 {name} 已被另一个系统注册——系统名必须唯一"
        return false
    }

    // 具名 wrapper + init 注册（范式同 Step 3）
    ...
    return true
}
```

---

## Step 5 — AOT 覆盖自动扫描

`Src/World/xmake.lua` 的 `Rules.das_aot` 调用处，或 `Rules.das_aot` 内部 before_build，增加自动扫描：

```lua
-- 扫描 Script/**/*.das，含注解的自动加入 AOT 批次
local autoExtra = {}
for _, f in ipairs(os.files(path.join(scriptRoot, "**/*.das"))) do
    local fp = io.open(f, "r")
    if fp then
        local content = fp:read("*a")
        fp:close()
        if content:find("%[msg_handler%]") or content:find("%[game_event%]")
           or content:find("%[game_system%]") then
            local rel = path.relative(f, scriptRoot)
            table.insert(autoExtra, rel)
        end
    end
end
-- 并入 extra_aot 清单（去重 + 保持既有 main.das 入口）
```

**验证**：新增一个 `Script/World/handlers.das`（含 `[msg_handler]`），Release 构建，确认其 `.das.cpp` 被生成且 `AOT coverage` 仍 100%。

---

## Step 6 — 全量验证清单

| # | 用例 | 期望 | 构建 |
|---|---|---|---|
| 1 | debug 全 target 构建 | 通过 | debug |
| 2 | WorldServer 启动，`RebindFunctions ... DispatchMsg=ok` | 日志正常 | debug |
| 3 | Move 消息 → `handle_move_test` 命中 | 日志出现 | debug |
| 4 | 热重载改 handler → 注册表重建 | 不崩、handler 生效 | debug |
| 5 | 同一 MSG 两个 handler → 编译报「已被注册」 | 编译错误 | debug |
| 6 | `[game_system(interval=1)]` → 编译报「须 float」 | 编译错误 | debug |
| 7 | `[msg_handler] def f(sid; req: NotInProto)` → 编译报「不在 MsgID.proto」 | 编译错误 | debug |
| 8 | Release 构建 + AOT coverage 100% | 含 `reg` 前缀符号 | release |
| 9 | 新增 handler 文件 → 自动进 AOT 批次 | coverage 仍 100% | release |
| 10 | 负向：`MsgTypeToID("Unknown")` 在宏外调用返回 -1（探针验证） | -1 | debug |

---

## 易错点速查

1. **`-1` 哨兵**：宏侧 `if (id < 0)` 必须报编译错，**严禁**直接 `uint(id)` 注册巨值假键
2. **`[macro_function]` 不能用**：只对 das 侧函数有意义，C++ addExtern 直接绑定即可
3. **查重表三键空间**：`"msg:"`/`"evt:"`/`"sys:"` 前缀，避免 `uint16` 与 `int64` 撞键
4. **查重表只被宏期引用**：若运行期函数引用会进 AOT surface，`remove_unused_symbols` 才裁剪
5. **MacroCommon 助手要 public**：三宏是独立模块，`def private` 不跨模块
6. **目标模块用 `compiling_module()`**：不硬编码，宏跑在哪个模块就写哪个模块
7. **wrapper 与 handler 同模块**：`_::` 前缀 + `privateFunction` 要求同模块，跨模块解析失败
8. **`require daslib/xxx` 斜杠**：统一官方斜杠语法，不用点号
9. **AOT 覆盖**：handler 文件必须是 AOT 批次成员（入口或 extra_aot 或自动扫描命中）
10. **`add_function` 返回 bool 要检查**：重复 mangled name 返回 false + `DAS_FATAL_ERROR`
