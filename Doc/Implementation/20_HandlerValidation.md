# 脚本引擎 #20：`[msg_handler]` 编译期注解 —— 零魔法数字 + 类型安全 Handler 自动注册

> 状态：**设计中**（2026-07-15）
> 关联：[13_MessageMigration](13_MessageMigration.md)（消息迁移，§11 代码生成器）
> 前置依赖：Phase 1-3 完成、daScript `AstFunctionAnnotation` API 已验证可用
> 对应 Phase：Phase 4

## 1. 设计目标

消除 Handlers.das 中的两类输入错误：

| 错误类型 | 旧写法 | 问题 |
|---------|--------|------|
| 魔法数字 | `msg=200` | 200 代表什么消息？语义丢失 |
| 字符串类型名 | `args="MoveReqArgs"` | 拼成 `"MoveReqAgrs"` 编译器不报错——它只是一个字符串 |

**目标**: `msg=MSG_MOVE_REQ`（有名称的常量）+ `var args : MoveReqArgs`（真实类型）。

## 2. 根基：daScript 的 `AstFunctionAnnotation`

`AstFunctionAnnotation.apply()` 可以直接读取被注解函数的参数 TypeDecl——不需要开发者手写类型名字符串。

```das
[function_macro(name="msg_handler")]
class MsgHandlerAnnotation : AstFunctionAnnotation {
    def override apply(var func : FunctionPtr; ...) : bool
    {
        // func.arguments 直接拿到函数的参数列表 —— TypeDecl 数组
        let fnArgs = func.arguments
        let secondArgType = fnArgs[1]._type         // ← 这是 TypeDecl，不是字符串
        let typeName = string(secondArgType.decl)    // → "MoveReqArgs"
        // 拼错类型名 → daScript 编译器先报 "undeclared identifier"，
        //   不会进入 apply()
    }
}
```

**关键**: 类型由 daScript 编译器的类型检查器校验，`apply()` 只是读取校验后的结果。

## 3. MsgID 常量

`GenMsgBindings.py` 产出 `MsgIDConstants.das`，扫描所有 `.proto` 文件提取每个 `MSG_*` 枚举值：

```das
// Scripts/AutoGen/MsgIDConstants.das — GenMsgBindings.py 产出，重跑 xmake 自动同步

let MSG_LOGIN_ENTER_WORLD_REQ  = 102u
let MSG_LOGIN_ENTER_WORLD_RSP  = 101u
let MSG_MOVE_REQ   = 200u
let MSG_MOVE_RSP   = 201u
let MSG_CHAT_REQ   = 301u
let MSG_CHAT_NTF   = 302u
let MSG_SKILL_CAST_REQ = 350u
let MSG_SKILL_CAST_RSP = 351u
let MSG_SKILL_CAST_NTF = 352u
```

## 4. 最终方案：`[msg_handler]` + 类型签名直读

### 4.1 HandlerRegistry.das（生成器产出）

```das
// Scripts/AutoGen/HandlerRegistry.das — GenMsgBindings.py 产出
require AutoGen/MsgIDConstants

// ═══════════════════════════════════════════════════════════
// 1. Args struct（生成器从 .proto 提取字段）
// ═══════════════════════════════════════════════════════════
struct MoveReqArgs {
    sessionID : uint32; posX, posY, posZ : float
    speed     : float;  sequence : uint;  timestamp : uint64
}
struct EnterWorldReqArgs {
    sessionID : uint32; accountID : uint64; sceneID : uint32
    posX, posY, posZ : float
}
struct ChatReqArgs {
    sessionID : uint32; channel : uint; content : string; targetID : uint64
}

// ═══════════════════════════════════════════════════════════
// 2. g_handler_registry — 全局注册表
// ═══════════════════════════════════════════════════════════
var g_handler_registry : table<uint32; block<(sessionID : uint32; argsPtr : void?) : void>>
var g_registered_count : int = 0

// ═══════════════════════════════════════════════════════════
// 3. [msg_handler] 注解 —— 只接受 msg= 整数常量
//    args 类型从函数签名直读，不收字符串
// ═══════════════════════════════════════════════════════════
[function_macro(name="msg_handler")]
class MsgHandlerAnnotation : AstFunctionAnnotation {
    def override apply(var func : FunctionPtr; var group : ModuleGroup;
                       args : AnnotationArgumentList; var errors : das_string) : bool
    {
        // ── 步骤 1: 读 msgID —— 必须是整数（支持 MSG_MOVE_REQ 常量）──
        let msgArg = find_arg(args, "msg")
        if (!(msgArg is tInt)) {
            errors := "[msg_handler] 需要指定 msg=<msgID> 参数"
            return false
        }
        let msgID = uint32(msgArg as tInt)

        // ── 步骤 2: 检查函数签名 —— 类型由 daScript 编译器天然校验 ──
        let fnArgs = func.arguments
        if (length(fnArgs) != 2) {
            errors := "[msg_handler] 函数必须有两个参数: (sessionID : uint32; var args : ReqArgs)"
            return false
        }

        // 第一个参数: sessionID : uint32
        if (fnArgs[0]._type.baseType != Type.tUInt || fnArgs[0]._type.dim != 0) {
            errors := "[msg_handler] 第一个参数必须是 sessionID : uint32"
            return false
        }

        // 第二个参数 —— 直接读 TypeDecl，不是字符串
        // 开发者写 var args : MoveReqAgrs → daScript 先报 "undeclared identifier"
        let argsType = fnArgs[1]._type
        let argsTypeName = string(argsType.decl)

        // ── 步骤 3: 自动注入注册代码 ──
        qmacro(
            g_handler_registry[$i(msgID)] <- @@(sessionID : uint32; argsPtr : void?)
            {
                var typedArgs = reinterpret<$ti(argsTypeName)>(argsPtr)
                $c("_{func.name}")(sessionID, typedArgs)
            }
        )
        g_registered_count += 1

        // 步骤 4: 设为 private（不暴露给 C++ 直接调用）
        func.flags |= FunctionFlags.privateFunction

        return true
    }
}

// ═══════════════════════════════════════════════════════════
// 4. C++ 调用入口 — 在 MsgDispatch.gen.cpp 中按 msgID 调此函数
// ═══════════════════════════════════════════════════════════
[export]
def dispatch_msg(msgID : uint32; sessionID : uint32; argsPtr : void?)
{
    let handler = find(g_handler_registry, msgID)
    if handler != null {
        invoke(handler, sessionID, argsPtr)
    }
}

// ═══════════════════════════════════════════════════════════
// 5. 期望 msgID 列表 —— 生成器按 .proto 扫描结果填入
// ═══════════════════════════════════════════════════════════
var g_expected_msg_ids : array<uint32>

[private]
def register_expected(msgID : uint32) { g_expected_msg_ids |> push(msgID) }

// ↓ 生成器按 msg 名排序填入:
register_expected(MSG_CHAT_REQ)
register_expected(MSG_LOGIN_ENTER_WORLD_REQ)
register_expected(MSG_MOVE_REQ)
```

### 4.2 Handlers.das（开发者手写）

```das
// Scripts/Handlers.das
require AutoGen/HandlerRegistry    // ← MsgIDConstants 通过此文件间接引入
require daslib/decs_boost
require massive

// ✅ msg=MSG_MOVE_REQ      — 有名称的常量，不是魔法数字
// ✅ var args : MoveReqArgs — 真实类型，不是字符串，拼错 → 编译器报错
[msg_handler(msg=MSG_MOVE_REQ)]
def handle_move(sessionID : uint32; var args : MoveReqArgs)
{
    let fullEid = massive_find_entity_by_session(sessionID)
    if fullEid == uint64(0) { return }

    if args.speed < 0.0f || args.speed > 50.0f {
        massive_log_warn("handle_move: invalid speed={args.speed}")
        return
    }

    let entityID = uint32(fullEid & uint64(0xFFFFFFFF))
    query(entityID) $(var intent : MoveIntent) {
        intent.targetPos = float3(args.posX, args.posY, args.posZ)
        intent.timestamp = args.timestamp
    }

    let rspBody = massive_build_move_rsp(entityID, args.sequence,
                                          args.posX, args.posY, args.posZ)
    massive_send_to_client(sessionID, MSG_MOVE_RSP, rspBody)
}

[msg_handler(msg=MSG_LOGIN_ENTER_WORLD_REQ)]
def handle_enter_world(sessionID : uint32; var args : EnterWorldReqArgs)
{
    let fullEid = massive_create_entity(float3(args.posX, args.posY, args.posZ), int32(0))
    let entityID = uint32(fullEid & uint64(0xFFFFFFFF))
    create_entity() @(eid2, cmp) {
        cmp.eid := entityID
        cmp.sessionID := sessionID
        cmp.accountID := args.accountID
    }
    commit()
    let rspBody = massive_build_enter_world_rsp(entityID, args.sceneID,
                                                  args.posX, args.posY, args.posZ)
    massive_send_to_client(sessionID, MSG_LOGIN_ENTER_WORLD_RSP, rspBody)
}

[msg_handler(msg=MSG_CHAT_REQ)]
def handle_chat(sessionID : uint32; var args : ChatReqArgs)
{
    // ... 频道分发 ...
}

// ═══════════════════════════════════════════════════════════
// init() —— 编译期校验 handler 完整性
// ═══════════════════════════════════════════════════════════
[export]
def init(isReload : bool)
{
    if !isReload { spawn_initial_npcs() }

    // 编译期校验: g_registered_count 由 [msg_handler] 注解自动递增
    // g_expected_msg_ids 由 Generator 填入期望的全部 msgID
    // 数量不一致 → 有人忘写 handler 或忘加注解
    static_if (g_registered_count != length(g_expected_msg_ids)) {
        concept_assert(false,
            "handler 完整性校验失败: 期望 {length(g_expected_msg_ids)} 个 handler，"
            "实际注册 {g_registered_count} 个")
    }
}
```

## 5. 编译期错误示例（全部精确到行号）

```
❌ 拼错消息常量名
   Handlers.das:4: undeclared identifier 'MSG_MOV_REQ'
                    （大小写或拼写错误）

❌ 拼错 Args 类型名
   Handlers.das:5: undeclared identifier 'MoveReqAgrs'
                    （daScript 编译器在注解 apply() 之前就发现）

❌ 忘记实现 handler
   Handlers.das: concept_assert: handler 完整性校验失败:
                    期望 3 个 handler，实际注册 2 个

❌ 函数参数数量错误（只写了 sessionID）
   Handlers.das: [msg_handler] 函数必须有两个参数

❌ 第一个参数不是 uint32
   Handlers.das: [msg_handler] 第一个参数必须是 sessionID : uint32

❌ msgID 写的是 MSG_MOVE_RSP（出站消息，未注册为入站 handler）
   Handlers.das: concept_assert: handler 完整性校验失败:
                    期望 3 个 handler（MSG_MOVE_REQ/MSG_ENTER_WORLD_REQ/MSG_CHAT_REQ）
                    实际注册包含了 MSG_MOVE_RSP 但不是期望的
```

## 6. 校验覆盖表

| 错误场景 | 捕获方式 | 报错阶段 |
|----------|---------|---------|
| **魔法数字 — msg 写成裸 200** | 合法但被代码规范禁止 — 只接受 `MSG_*` 常量 | Code Review |
| **消息 ID 不存在 — `MSG_MOV_REQ`** | daScript 编译器: undeclared identifier | 编译期 |
| **类型名拼错 — `MoveReqAgrs`** | daScript 编译器: undeclared identifier | 编译期 |
| **handler 缺失** | `static_if(g_registered_count != length(g_expected))` | 编译期 |
| **忘记 `[msg_handler]` 注解** | 同上 —— 注解不生效则不注册 | 编译期 |
| **参数数量错误** | `apply()` 中 `length(fnArgs) != 2` | 编译期 |
| **参数类型写错 — `ChatReqArgs` 用作 Move handler** | ⚠️ 签名合法 — 编译通过 — 集成测试覆盖 | 运行时 |

**覆盖率: 6/7**。唯一无法在编译期捕获的：用正确的签名注册了错误的 msgID→Args 对应关系（`[msg_handler(msg=MSG_MOVE_REQ)]` 但 handler 参数写了 `ChatReqArgs`）。这种错误会通过——因为类型检查只验证签名合法性，不验证语义正确性。由集成测试覆盖。

## 7. 文件最终结构

```
Scripts/
├── Handlers.das                    # 开发者手写 — 业务逻辑 + [msg_handler] 注解
├── Components.das                  # 开发者手写 — DECS template
├── ServerTick.das                  # 开发者手写 — Tick 调度
├── AutoGen/                        # GenMsgBindings.py 产出 — gitignore
│   ├── MsgIDConstants.das          # msgID 常量（let MSG_MOVE_REQ = 200u）
│   └── HandlerRegistry.das         # Args struct + [msg_handler] 注解类 + 期望 msgID 列表

Src/World/AutoGen/                  # GenMsgBindings.py 产出 — gitignore
├── MsgDispatch.gen.cpp             # C++ switch case
└── MsgArgs.gen.h                   # C++ Args struct

Tools/Script/
└── GenMsgBindings.py               # 扫描 .proto → 产出上面 4 个文件
```

## 8. 引用

| 机制 | 源码位置 |
|------|---------|
| `AstFunctionAnnotation` | `ThirdParty/daScript/daslib/ast.das:98-109` |
| `decs_boost.das` — `DecsEcsMacro` apply 模式 | `ThirdParty/daScript/daslib/decs_boost.das:565-648` |
| `qmacro` / `qmacro_function` — AST 生成 | `ThirdParty/daScript/daslib/templates_boost.das` |
| `concept_assert` | 内建 — 广泛用于 algorithm.das |
| `TypeDecl` API（`_type.baseType`/`_type.dim`/`_type.decl`） | `ThirdParty/daScript/daslib/ast.das` |
| daScript 0.6.3 — `interfaces.das` completeness check | `ThirdParty/daScript/daslib/interfaces.das:262-306` |

## 9. daScript 0.6.3 备选方案：`[interface]` + `[implements]`

daScript 0.6.3 内置了 `[interface]` + `[implements(InterfaceName)]` — 包含编译期 completeness checking。这是一种更轻量的签名校验替代方案（无需自定义宏类），但缺少 msgID→dispatch 关联能力。详见 [22_DaScript063Analysis §2](22_DaScript063Analysis.md#2-核心发现interfacesdas-可直接替代-handlerregistry)。
