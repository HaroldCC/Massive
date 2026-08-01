# 消息绑定与脚本入口（ScriptLayer_01_MsgBinding）

> 本篇覆盖「网络消息 → 脚本 handler」的完整链路，以及脚本入口的重构。
> 所有代码可照抄。涉及的 daScript API 均已对 `ThirdParty/daScript` 源码核实。
>
> 承接 `ScriptLayer_00_Overview.md`。

---

## 0. 本篇改动总览

| 项 | 旧 | 新 |
|---|---|---|
| 脚本入口 | `Script/ServerTick.das` | `Script/main.das` |
| `HandlerRegistry.das` | 生成器产出 | **手写**在 `Script/HandlerRegistry.das`（去 AutoGen） |
| `[msg_handler]` 宏 | 生成，靠 `g_msg_name_to_id` 生成表查 msgID | **手写**，编译期从 C++ 绑定的 `EMsgID` 枚举按命名约定推导 msgID |
| C++ 消息绑定 | 生成 | **继续生成**（`EMsgID` 枚举、`ManagedStructureAnnotation`、`Dispatch<Msg>Req`） |
| `Dispatch<Msg>Req` | 耦合 `WorldServer` + 调不存在的 `GetDispatchMsgFunction()` | 走 `DasLangEngine::GetIns()`，服务无关 |
| 生成器 | `GenMsgBindings.py`（同时产 C++ 和 das） | `GenMsgBindings.py` v2（仅产 C++，已重写验证） |

---

## 1. 链路全景

```
① 网络层收到消息 (msgID, sessionID, body, len)
② ScriptDispatchRegistry::Dispatch(msgID, sessionID, body, len)     [C++, O(1) 定长表]
③   → Dispatch<Msg>Req(sessionID, body, len)                        [C++, *.gen.cpp]
④        req.ParseFromArray(body, len)                              [protobuf 解析]
⑤        ctx->evalWithCatch(dispatch_msg, {msgID, sessionID, &req}) [进脚本]
⑥          dispatch_msg 查 g_handler_registry[msgID]               [das, HandlerRegistry.das]
⑦            → handle_move(sessionID, req)                          [das, 手写 handler]
```

- ②③④⑤ 在 **C++** 侧，其中 ③④ 由 `GenMsgBindings.py` 生成。
- ⑥⑦ 在 **das** 侧，手写。⑥ 的 `g_handler_registry` 由 `[msg_handler]` 宏在 `[init]` 阶段填充。

---

## 2. C++ 侧：生成器 v2（已完成）

`Tools/Script/GenMsgBindings.py` 已按本设计重写并验证。核心行为：

- 扫描 `Src/Proto/*.proto`，识别 `*Req`，计算依赖闭包。
- 每个闭包文件产 `Src/World/AutoGen/<Name>.gen.cpp`：
  - `MAKE_TYPE_FACTORY` + `ManagedStructureAnnotation`（消息/值类型绑定）
  - string/bytes → `addExternProperty(".`字段名")`；repeated → `_size` + 索引访问
  - `*Req` → `Dispatch<Msg>Req(sessionID, body, len)`
- 产 `ProtoBindIndex.gen.{h,cpp}`：`RegisterAllProtoMessageTypes` + `RegisterAllMsgDispatch` + `EnumerationEMsgID` 绑定 + `DAS_BIND_ENUM_CAST`。
- **不再产 das**。

### 2.1 生成的 Dispatch 函数（服务无关）

```cpp
// Src/World/AutoGen/Move.gen.cpp（片段，生成产物）
bool DispatchMoveReq(uint32 sessionID, const uint8 *body, size_t len)
{
    MMO::Proto::MoveReq req;
    if (!req.ParseFromArray(body, static_cast<int>(len)))
    {
        MMO::Log::Error("Move.gen: MoveReq parse failed, session={}", sessionID);
        return false;
    }

    das::Context     *ctx        = MMO::DasLangEngine::GetIns().GetScriptContext();
    das::SimFunction *fnDispatch = MMO::DasLangEngine::GetIns().GetDispatchFunc();
    if (nullptr == ctx || nullptr == fnDispatch)
    {
        return false;
    }

    vec4f callArgs[3] = {
        das::cast<uint32_t>::from(static_cast<uint32_t>(MMO::Proto::MSG_MOVE_REQ)),
        das::cast<uint32_t>::from(sessionID),
        das::cast<const MMO::Proto::MoveReq *>::from(&req),
    };
    ctx->evalWithCatch(fnDispatch, callArgs);
    return true;
}
```

**与旧版差异**（已由 diff 验证仅这些语义变化）：
- 签名去掉 `WorldServer &server`。
- `server.GetScriptContext()/GetDispatchMsgFunction()`（后者接口根本不存在）→ `DasLangEngine::GetIns().GetScriptContext()/GetDispatchFunc()`。
- `ctx->eval` → `ctx->evalWithCatch`（异常安全）。
- include 去掉 `World/WorldServer.h`，加 `ScriptEngine/DasEngine.h` + `Common/Log/Log.h`。

> ⚠️ **不要立刻用生成器覆写现有 `Src/World/AutoGen/*.gen.cpp`**：新 Dispatch 走 `DasLangEngine`，而 `WorldServer` 当前仍跑并行老路径、尚未接引擎。覆写会断当前构建。正确时机是本篇 §6 引擎接线完成后，一并 regen。

### 2.2 xmake 调用调整

`Src/World/xmake.lua` 的 `gen_msg_bindings` rule 里去掉 `--das-out` 参数（生成器已忽略它，但清理调用更干净）：

```lua
-- Src/World/xmake.lua，on_load 内
os.vrunv("python", {genScript, "--proto-dir", protoDir,
                    "--cpp-out", autogenDir})
-- 迁移完成、确认手写 HandlerRegistry.das 就位后，可跑一次带 --purge-legacy-das 清理旧 das 产物：
--   python Tools/Script/GenMsgBindings.py --proto-dir Src/Proto --cpp-out Src/World/AutoGen --purge-legacy-das
```

> `--purge-legacy-das` 是显式开关。默认不删 `Script/AutoGen/HandlerRegistry.das`，避免尚未迁移时误删（它是 gitignore 的生成产物，删了 git 无法恢复）。

---

## 3. das 侧：手写 `HandlerRegistry.das`

新位置：`Script/HandlerRegistry.das`（不再放 `AutoGen/`，因为手写）。这是本篇技术核心——`[msg_handler]` 宏改为**编译期从 C++ 绑定的 `EMsgID` 枚举按命名约定推导 msgID**，不依赖任何生成的查表。

### 3.1 可行性结论（已核实 daScript 源码）

- `[function_macro] apply()` 在**解析期**运行；native 模块在解析前已进 `program->library`（`ast_parse.cpp:814-817`）——**无「宏先于枚举注册」的时序风险**（C++ 枚举在 `Module::Initialize` / `BindFunctions` 阶段就绪，早于任何脚本编译）。
- 拿消息类型名：`func.arguments[1]._type` 是 handled type，`baseType == Type.tHandle && annotation != null`，`annotation.name == "MoveReq"`（`ast_typedecl.h:252`，现有代码已用）。
- 查枚举：`find_compiling_module("Common")`（`ast.das:993`）→ `module_find_enumeration(mod, "EMsgID")`（bound `module_builtin_ast.cpp:1619`）→ 遍历 `enu.list` 的 `EnumEntry{name, value:ExpressionPtr}`（`ast.h:162`）。
- 取值：`find_enum_value(enu, name):int64`（bound `module_builtin_ast.cpp:1389`）。
- **typo 防护**：`Enumeration::find` 未命中返回默认 0，与合法的 `MSG_NONE=0` 无法区分——所以**必须先扫 `enu.list` 确认 `name` 存在**，再取值。用 `macro_verify(cond, compiling_program(), at, msg)`（`macro_boost.das:18`）报编译错。
- `require`：仍需 `require Common`——handler 签名引用了绑定的**类型**（`MoveReq`、`EMsgID`），类型名解析需要 import；与查枚举正交。

### 3.2 完整文件

```das
// Script/HandlerRegistry.das — 消息分发注册（手写）
// [msg_handler] 宏在编译期从 C++ 绑定的 EMsgID 枚举按命名约定推导 msgID。
// 依赖 Common 模块提供的 EMsgID 枚举与各消息类型（MoveReq 等）。

options gen2
options indenting = 4

module HandlerRegistry

require daslib/ast
require daslib/ast_boost
require daslib/macro_boost
require daslib/strings_boost

require Common

// ── 运行期分发表：msgID → handler function ──
var g_handler_registry : table<uint; function<(sessionID : uint; msgPtr : void?) : void>>

// ── 命名约定：MoveReq → MSG_MOVE_REQ（与 GenMsgBindings.py camel_to_msg_id 同规则）──
// 规则：小写→大写前插 '_'（连续大写视作缩写不拆首字母之间），整体大写，前缀 MSG_。
def private camel_to_msg_id(name : string) : string {
    var out : array<uint8>
    let n = length(name)
    for (i in range(n)) {
        let c = character_at(name, i)
        // 是否在此字符前插入下划线：当前是大写，且（前一个是小写）或（前一个是大写且下一个是小写）
        if (i > 0 && is_upper(c)) {
            let prev = character_at(name, i - 1)
            var insert = false
            if (!is_upper(prev)) {
                insert = true
            } elif (i + 1 < n && !is_upper(character_at(name, i + 1))) {
                insert = true
            }
            if (insert) {
                out |> push(uint8('_'))
            }
        }
        out |> push(to_upper_char(c))
    }
    return "MSG_" + string(out)
}

def private is_upper(c : int) : bool {
    return c >= int('A') && c <= int('Z')
}

def private to_upper_char(c : int) : uint8 {
    if (c >= int('a') && c <= int('z')) {
        return uint8(c - int('a') + int('A'))
    }
    return uint8(c)
}

// ── [msg_handler] 注解 ──
[function_macro(name="msg_handler")]
class MsgHandlerAnnotation : AstFunctionAnnotation {
    def override apply(var func : FunctionPtr; var group : ModuleGroup;
                       args : AnnotationArgumentList; var errors : das_string) : bool {
        // 步骤 1: 签名至少 (sessionID : uint; req : <Msg>)
        if (length(func.arguments) < 2) {
            errors := "[msg_handler] 函数至少需要两个参数: (sessionID : uint; req : <消息类型>)"
            return false
        }
        if (func.arguments[0]._type.baseType != Type.tUInt || func.arguments[0]._type.fixedDim != 0) {
            errors := "[msg_handler] 第一个参数必须是 sessionID : uint"
            return false
        }

        // 步骤 2: 第二个参数必须是消息类型（handled type），取类型名
        let msgType = func.arguments[1]._type
        if (msgType.baseType != Type.tHandle || msgType.annotation == null) {
            errors := "[msg_handler] 第二个参数必须是消息类型（如 req : MoveReq），实际不是 handled type"
            return false
        }
        let msgTypeName = string(msgType.annotation.name)   // "MoveReq"
        let msgIdName   = camel_to_msg_id(msgTypeName)       // "MSG_MOVE_REQ"

        // 步骤 3: 定位 C++ 绑定的 EMsgID 枚举
        let cmod = find_compiling_module("Common")
        if (cmod == null) {
            errors := "[msg_handler] 未找到 Common 模块——handler 文件需 require Common"
            return false
        }
        let enu = module_find_enumeration(cmod, "EMsgID")
        if (enu == null) {
            errors := "[msg_handler] Common 模块未绑定 EMsgID 枚举"
            return false
        }

        // 步骤 4: typo 防护——必须先确认成员存在（find_enum_value 未命中返回 0，与 MSG_NONE=0 无法区分）
        var found = false
        for (e in enu.list) {
            if (e.name == msgIdName) {
                found = true
                break
            }
        }
        if (!found) {
            errors := "[msg_handler] 消息类型 {msgTypeName} 推导出的 {msgIdName} 不在 EMsgID 中——检查命名或 .proto 是否存在对应 *Req"
            return false
        }
        let msgID = uint(find_enum_value(enu, msgIdName))

        // 步骤 5: 注入注册代码到 [init] 块——运行期 context 才会执行
        //   apply() 在编译期 macroContext 运行，直接改全局只影响宏 context；
        //   必须用 qmacro_expr 生成语句塞进 [init]，才会在运行时 _scriptCtx 执行。
        var initBlk <- setup_call_list("msg_handler`init", func.at, true, true)
        initBlk.list |> push(qmacro_expr(${
            g_handler_registry[$v(msgID)] = @@(sessionID : uint; msgPtr : void?) {
                let typedMsg = unsafe(reinterpret<$t(msgType)?> msgPtr)
                $c("_::{func.name}")(sessionID, *typedMsg)
            }
        }))
        func.flags.privateFunction = true
        return true
    }
}

// ── C++ 唯一调用入口 ──
[export]
def dispatch_msg(msgID : uint; sessionID : uint; msgPtr : void?) {
    g_handler_registry |> get(msgID) $(handler) {
        invoke(handler, sessionID, msgPtr)
    }
}
```

### 3.3 说明与取舍

- **为何不再有 `g_expected_handler_count` / `validate_handler_registry`**：旧版靠生成的期望数量做完整性校验。改手写后没有「生成器扫描出的期望数量」这个外部真相源；漏写 handler 表现为该消息静默不处理。若你想保留完整性校验，可在 §5 的 `main.das` 里对关键消息用 `assert(key_exists(...))` 显式点名，或保留一个手写的期望集合。**默认去掉**——手写场景下拼错类型名会被宏的步骤 4 直接编译期拦截，比运行期计数更早。
- **`camel_to_msg_id` 的命名规则**必须与 `GenMsgBindings.py:camel_to_msg_id` 完全一致，否则推导的 `MSG_xxx` 查不到。两处规则相同：`LoginEnterWorldReq → MSG_LOGIN_ENTER_WORLD_REQ`、`MoveReq → MSG_MOVE_REQ`。已对齐。
- **`is_upper` / `to_upper_char` 自写**：用 `character_at`（`strings` builtin，O(n) 但宏在编译期只跑一次、名字短，可接受）逐字符处理。若嫌 `character_at` 慢可换 `peek_data`，但编译期一次性转换无需优化。
- **`module_find_enumeration`** 优于遍历 `for_each_enumeration`：直接按名取，`module_builtin_ast.cpp:1619` 已绑定。

---

## 4. das 侧：手写 handler 业务

`Script/Handlers.das`（手写，业务逻辑）。注意 `require Common`（提供类型 + 枚举）与 `require HandlerRegistry public`（提供 `[msg_handler]` 宏）。

```das
// Script/Handlers.das — 消息处理业务逻辑（手写）
options gen2
options indenting = 4

require HandlerRegistry public
require Common

[msg_handler(msg="MoveReq")]
def handle_move(sessionID : uint; req : MoveReq) {
    if (req.speed < 0.0 || req.speed > 50.0) {
        LogWarn("handle_move: invalid speed={req.speed} session={sessionID}")
        return
    }
    LogInfo("handle_move: seq={req.sequence} pos=({req.position.x},{req.position.y},{req.position.z})")
}

[msg_handler(msg="HeartbeatReq")]
def handle_heartbeat(sessionID : uint; req : HeartbeatReq) {
    LogInfo("handle_heartbeat: session={sessionID} client_time={req.client_time}")
}

[msg_handler(msg="LoginAuthReq")]
def handle_login_auth(sessionID : uint; req : LoginAuthReq) {
    let user = req.username
    LogInfo("handle_login_auth: session={sessionID} user={user}")
}

[msg_handler(msg="LoginEnterWorldReq")]
def handle_login_enter_world(sessionID : uint; req : LoginEnterWorldReq) {
    LogInfo("handle_login_enter_world: session={sessionID} nonce={req.nonce}")
}
```

> 注：`msg="MoveReq"` 这个注解参数现在**可选/仅作可读标注**——宏实际从 `req : MoveReq` 的类型推导 msgID。若你希望强制二者一致（防 handler 写错类型），可在宏步骤 2 后加一条：若 `find_arg(args,"msg")` 存在且 `!= msgTypeName` 则报错。本文默认**以类型为准**，注解参数留作文档性质；是否强制留给你定。

---

## 5. das 侧：入口 `main.das`

`Script/ServerTick.das` → `Script/main.das`。入口只保留 `Init` / `Update` 两个导出；`dispatch_msg` 在 `HandlerRegistry.das`。

```das
// Script/main.das — 脚本入口（原 ServerTick.das）
options gen2
options indenting = 4

require Common
require Handlers    // 引入 [msg_handler]，触发 handler 注册的 [init] 注入

[export]
def Init() {
    LogInfo("script init")
    // 业务初始化（World/Social 后续在此拼装）
}

[export]
def Update(sceneID : uint; dt : float) {
    // 每帧逻辑（当前为空；业务后续拼装）
}
```

- 引擎的 `RebindFunctions` 用 `findFunction("Init")` / `"Update"` / `"dispatch_msg"`（精确大小写 strcmp，已核实 `context.cpp:606`）。命名必须完全一致。
- `require Handlers` 是必须的——它把 `[msg_handler]` 的 `[init]` 注册代码拉进本 program，`simulate` 时运行填充 `g_handler_registry`。

---

## 6. C++ 侧：`IDasLangHost` / `ScriptDispatchRegistry` / 引擎接线

### 6.1 `IDasLangHost` 保持接口纯粹

`Src/ScriptEngine/IDasHost.h`（当前已存在，接口无需大改）：

```cpp
#pragma once
#include "daScript/simulate/simulate.h"
#include "Common/Core/Types.h"

namespace MMO
{
    class IDasLangHost
    {
    public:
        virtual ~IDasLangHost() = default;
        // 脚本回调宿主：把服务器要发的包投递给客户端
        virtual void SendRawToClient(uint32 sessionID, uint32 msgID, const uint8 *data, size_t len) = 0;
    };
} // namespace MMO
```

> 注意：把 `GetScriptContext()` / `GetDispatchFunc()` 从 `IDasLangHost` 移除——这两个是**引擎**的能力，不是宿主回调。生成的 `Dispatch<Msg>Req` 已改为直接问 `DasLangEngine::GetIns()`，不再经宿主。类名统一为 `IDasLangHost`（去掉笔误的 `IDasLangtHost`）。

### 6.2 `ScriptDispatchRegistry` 不变

`Src/World/ScriptDispatchRegistry.h/.cpp` 保持现状（`Register(msgID, fn)` + `Dispatch(...)` O(1) 定长表），但分发函数签名同步去掉 `WorldServer &`：

```cpp
// Src/World/ScriptDispatchRegistry.h（修改点）
using ScriptDispatchFn = bool (*)(uint32 sessionID, const uint8 *body, size_t len);

class ScriptDispatchRegistry
{
public:
    static void Register(uint32 msgID, ScriptDispatchFn fn);
    static bool Dispatch(uint32 sessionID, uint32 msgID, const uint8 *body, size_t len);
private:
    static std::array<ScriptDispatchFn, kMaxHandlers> &Table();
};
```

```cpp
// Src/World/ScriptDispatchRegistry.cpp（Dispatch 去掉 server 转发参数）
bool ScriptDispatchRegistry::Dispatch(uint32 sessionID, uint32 msgID,
                                      const uint8 *body, size_t len)
{
    if (msgID >= kMaxHandlers || !Table()[msgID])
    {
        return false;
    }
    return Table()[msgID](sessionID, body, len);
}
```

调用点（`WorldServer::OnMessage`）相应改为 `ScriptDispatchRegistry::Dispatch(sessionID, msgID, body, len)`。

### 6.3 服务侧在 Load 后注册分发（分层解耦的关键）

`RegisterAllMsgDispatch()`（生成产物，注册所有 `Dispatch<Msg>Req` 进表）由**服务侧**在脚本 `Load` 成功后调用，**不在引擎内**：

```cpp
// Src/World/WorldServer.cpp（InitScriptEngine 重构后示意）
#include "World/AutoGen/ProtoBindIndex.gen.h"   // 仅 World 侧 include，引擎不 include

bool WorldServer::InitScriptEngine()
{
    auto &engine = DasLangEngine::GetIns();

    DasLangEngineConfig cfg;                 // 从 ConfigLoader 填充（见 03 篇）
    cfg.dasLangRoot = "Script";
    cfg.mode        = EScriptMode::Develop;  // 或 Release
    if (!engine.Initialize(cfg, this /*IDasLangHost*/, &_moduleProvider))
    {
        return false;
    }
    if (!engine.Load("main.das"))            // 入口更名
    {
        return false;
    }
    RegisterAllMsgDispatch();                // ← 服务侧注册，引擎无关
    return true;
}
```

`_moduleProvider` 是 World 实现的 `IDasLangModuleProvider`。它的 `CreateModules(group)` 负责建 `WorldModule`（含 `RegisterAllProtoMessageTypes(mod, lib)` 灌入 `EMsgID` 与消息类型）。`WorldModule` 本轮不实现，仅约定接口（见 §7）。

---

## 7. `IDasLangModuleProvider` 与服务专用模块（本轮仅约定接口）

`Src/ScriptEngine/IDasModuleProvider.h`（已存在）职责回顾：

```cpp
class IDasLangModuleProvider
{
public:
    virtual ~IDasLangModuleProvider() = default;
    virtual void CreateModules(das::ModuleGroup &group) = 0;          // 建服务专用 native 模块
    virtual void OnContextSwapped(std::shared_ptr<das::Context> ctx) = 0; // swap 后通知
    virtual void OnPrevTick(float dt) = 0;                            // tick 前转发 dt 等
    virtual void DrainTimers() = 0;                                   // swap 前清定时器
    virtual const char *MainScriptFile() const = 0;                  // 入口脚本（"main.das"）
    virtual const char *ModuleName() const = 0;                      // 服务模块名（"world"）
};
```

**World 侧未来的实现骨架**（本轮不写，仅示意约定）：

```cpp
// Src/World/Script/WorldModuleProvider.h（未来）——本轮不实现
class WorldModuleProvider : public IDasLangModuleProvider
{
public:
    void CreateModules(das::ModuleGroup &group) override
    {
        // 建 WorldModule（require "world"），BindFunctions 里：
        //   RegisterAllProtoMessageTypes(*_worldModule, lib);   // EMsgID + 消息类型
        //   + World 专用绑定（Scene/战斗/AOI/定时器...）
        // group.addModule(_worldModule.get()); // 若 WorldModule 需进 group
    }
    // ... OnContextSwapped / OnPrevTick / DrainTimers ...
    const char *MainScriptFile() const override { return "main.das"; }
    const char *ModuleName() const override { return "world"; }
};
```

> `DasCommonModule`（`"Common"`）由引擎自身持有（构造即注册），**无需** provider 再 `addModule`——native 模块经环境全局表解析，编译时自动进 libGroup（已核实 `ast_parse.cpp:504`）。`EMsgID` 与消息类型放哪个模块取决于分层：公共消息进 `Common`、服务专用消息进 `WorldModule`。本轮的 `RegisterAllProtoMessageTypes` 先随 World 走（沿用现状），后续分层时再拆。

---

## 8. 迁移步骤清单（可执行）

1. **C++**：改 `ScriptDispatchRegistry` 签名去 `WorldServer &`（§6.2）；`IDasHost.h` 精简为纯回调、类名 `IDasLangHost`（§6.1）。
2. **das**：新建手写 `Script/HandlerRegistry.das`（§3.2）、`Script/Handlers.das`（§4）、`Script/main.das`（§5）；删旧 `Script/ServerTick.das`。
3. **引擎**：按 02 篇修 `DasLangEngine`（F1/F2/F7 + 接线），`RebindFunctions` 用 `"dispatch_msg"`（不是 `"DispatchMsg"`）。
4. **生成器**：确认 `main.das` 能编译通过后，用 v2 重新生成 `Src/World/AutoGen/*.gen.cpp`（新 Dispatch 走引擎）；`xmake.lua` 去 `--das-out`。
5. **清理**：迁移确认无误后 `--purge-legacy-das` 删旧 `Script/AutoGen/HandlerRegistry.das`。
6. **验证**：Develop 模式启动 → `Init` 打日志 → 发一条 `MoveReq` → `handle_move` 收到且字段正确。

> `main.das` 里 `require Common` / `require Handlers`：确保 `Common` 模块（`DasCommonModule` + `EMsgID`/消息类型绑定）在编译前已注册进环境（引擎 `Initialize` 完成）。宏在解析期查 `EMsgID` 无时序问题（§3.1 已核实）。
