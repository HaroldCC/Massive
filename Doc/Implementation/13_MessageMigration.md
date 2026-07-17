# 脚本引擎 #13：消息迁移——C++ Handler → 脚本 Handler

> 状态：**设计中**（2026-07-15）
> 关联：[09_ScriptEngine](09_ScriptEngine.md)（总纲 §7、§10）、[10_BridgeModule](10_BridgeModule.md)（MassiveModule）
> 前置依赖：Phase 2 完成（MassiveModule 核心函数可用、EntityManager 可用、Components.das 已定义）
> 对应 Phase：Phase 3（消息迁移到脚本）

## 1. 目标

将当前 C++ Handler（`EnterWorldHandler`、`MoveHandler`）的业务逻辑迁移到 DasLang 脚本中。C++ 侧只做 **Protobuf 反序列化 + 原生类型投递**，业务逻辑全部在脚本中。

## 2. 当前 C++ Handler 状态

| Handler | 文件 | 复杂度 | 迁移策略 |
|---------|------|--------|---------|
| EnterWorldHandler | `Src/World/Handler/EnterWorldHandler.cpp`（234 行） | 中等——含 SessionToken 验证、重连、CryptoSession 初始化 | 仅将"创建 entity + 构建响应"迁移到脚本；验证逻辑保留 C++ |
| MoveHandler | `Src/World/WorldServer.cpp`（内联 lambda，~30 行） | 简单——速度校验 + 回包 | 完整迁移到脚本 |

## 3. 消息流

```
Client → Gate → World IO Thread → Per-Session inbox → LogicThread::ProcessMessages()
                                                         │
                                            ┌────────────┘
                                            ▼
                                    控制消息（msgID < kUserMsgIDStart）
                                      → C++ 处理（DisconnectNtf / SessionRebind）
                                            │
                                    业务消息（msgID ≥ kUserMsgIDStart）
                                      │
                              C++ 侧 Protobuf 反序列化
                                      │
                                      ▼
                              das_invoke(脚本 handler, 原生类型参数)
                                      │
                              [DECS] handle_xxx(sessionID, field1, field2, ...)
                                      │
                              DECS component 读写 → commit()
                              massive_send_to_client() 出站
```

## 4. C++ 侧分派实现

### 4.1 WorldServer 修改

```cpp
// WorldServer.h 新增成员
class WorldServer
{
    // ... 已有成员 ...

    // ── 脚本引擎 ──
    std::unique_ptr<das::Context>  _scriptCtx;
    std::unique_ptr<EntityManager> _entityMgr;
    bool                           _useScriptHandlers = true;  // feature flag
};
```

```cpp
// WorldServer.cpp — OnMessage 修改
void WorldServer::OnMessage(uint32 sessionID, WorldSession &ws, const LogicMessage &msg)
{
    (void)ws; // LogicThread 独占

    // 控制消息留在 C++ 处理
    if (msg.msgID < kUserMsgIDStart)
    {
        OnControlMessage(msg.msgID, msg.body.Data(), msg.body.Size());
        return;
    }

    // 业务消息——Protobuf 预解析 + 脚本 dispatch
    if (_useScriptHandlers)
    {
        DispatchToScript(sessionID, msg);
    }
    else
    {
        // Fallback: 传统 C++ 分派
        _dispatcher.Dispatch(sessionID, msg.msgID, msg.body.Data(), msg.body.Size());
    }
}
```

### 4.2 分派实现

```cpp
void WorldServer::DispatchToScript(uint32 sessionID, const LogicMessage &msg)
{
    switch (msg.msgID)
    {
    case Proto::MSG_MOVE_REQ:
    {
        Proto::MoveReq req;
        if (!req.ParseFromArray(msg.body.Data(), static_cast<int>(msg.body.Size())))
        {
            Log::Warn("MoveReq parse failed session={}", sessionID);
            return;
        }
        auto fn = _scriptCtx->findFunction("handle_move");
        if (fn)
        {
            das_invoke<void>::invoke(_scriptCtx.get(), nullptr, fn,
                sessionID,
                req.position().x(), req.position().y(), req.position().z(),
                req.speed(), req.sequence(), req.timestamp());
        }
        break;
    }

    case Proto::MSG_LOGIN_ENTER_WORLD_REQ:
    {
        Proto::LoginEnterWorldReq req;
        if (!req.ParseFromArray(msg.body.Data(), static_cast<int>(msg.body.Size())))
        {
            Log::Warn("EnterWorldReq parse failed session={}", sessionID);
            return;
        }
        // EnterWorld 的验证逻辑（SessionToken 校验）保留 C++ 侧
        // 验证通过后投递到脚本——由 ProcessUnroutedMessages 中完成
        // 在验证成功后调用脚本 handler
        break;
    }

    default:
        Log::Debug("Unhandled business msgID={}", msg.msgID);
        break;
    }
}
```

### 4.3 EnterWorld 的特殊处理

EnterWorld 有其特殊性——消息在路由前到达（`_unroutedQueue`），需要在 C++ 侧完成 SessionToken 验证和 CryptoSession 初始化后才能投递给脚本。流程如下：

```cpp
// WorldServer::ProcessUnroutedMessages() —— 修改后
void WorldServer::ProcessUnroutedMessages()
{
    // ... Drain _unroutedQueue ...

    for (auto &msg : batch)
    {
        // 1. C++ 侧：SessionToken 验证 + CryptoSession 初始化（保留 C++ 逻辑）
        Proto::LoginEnterWorldReq req;
        req.ParseFromArray(msg.body.Data(), static_cast<int>(msg.body.Size()));

        auto tokenOpt = Crypto::SessionToken::FromBuffer(...);
        auto payloadOpt = Crypto::SessionTokenBuilder::Verify(lss, *tokenOpt);
        if (!payloadOpt) { /* 错误响应 */ continue; }

        // 2. 重连检查
        uint32 accountID = payloadOpt->accountId;
        // ... 重连逻辑保留 C++ ...

        // 3. 创建 WorldSession + CryptoSession（C++ 侧，不变）

        // 4. 投递到脚本 handler——由脚本完成 entity 创建 + DECS 初始化
        if (_useScriptHandlers)
        {
            auto fn = _scriptCtx->findFunction("handle_enter_world");
            if (fn)
            {
                das_invoke<void>::invoke(_scriptCtx.get(), nullptr, fn,
                    sessionID, accountID,
                    req.scene_id(),
                    req.pos_x(), req.pos_y(), req.pos_z());
            }
        }
        else
        {
            // Fallback: C++ Handler
            EnterWorldHandler::Handle(...);
        }
    }
}
```

## 5. 脚本侧实现

### 5.1 Handlers.das

```das
// Scripts/Handlers.das
require daslib/decs_boost
require massive

// ── Move Handler ──
[export]
def handle_move(sessionID : uint32; posX, posY, posZ : float;
                speed : float; sequence : uint32; timestamp : uint64)
{
    // 1. 查 session → entity
    let fullEid = massive_find_entity_by_session(sessionID)
    if fullEid == uint64(0) {
        massive_log_warn("handle_move: session not found")
        return
    }

    let entityID = extract_entity_id(fullEid)

    // 2. 服务器权威速度校验
    if speed < 0.0f || speed > 50.0f {
        massive_log_warn("handle_move: invalid speed={speed}")
        return
    }

    // 3. 写 MoveIntent（DECS 组件）
    query(entityID) $(var intent : MoveIntent) {
        intent.targetPos = float3(posX, posY, posZ)
        intent.timestamp = timestamp
    }

    // 4. 构建 MoveRsp protobuf → 发送
    let rspData = massive_build_move_rsp(entityID, sequence, posX, posY, posZ)
    massive_send_to_client(sessionID, MSG_MOVE_RSP, rspData)  // ← 常量由 GenMsgBindings.py 自动生成
}

// ── EnterWorld Handler ──
[export]
def handle_enter_world(sessionID : uint32; accountID : uint64;
                       sceneID : uint32; posX, posY, posZ : float)
{
    // 1. 创建 entity（EnTT + DECS 双创建）
    let fullEid = massive_create_entity(float3(posX, posY, posZ), int32(0))  // 0 = Player
    if fullEid == uint64(0) {
        massive_log_error("handle_enter_world: create entity failed")
        return
    }

    let entityID = extract_entity_id(fullEid)

    // 2. DECS 侧初始化——PlayerData 组件
    create_entity() @(eid2, cmp) {
        cmp.eid := entityID
        cmp.sessionID := sessionID
        cmp.accountID := accountID
    }
    commit()

    // 3. 绑定 session ↔ entity（C++ 侧映射表——由 C++ 在 das_invoke 返回后调用）
    //    这一步在 WorldServer::ProcessUnroutedMessages() 中完成

    // 4. 构建 EnterWorldRsp → 发送
    let rspData = massive_build_enter_world_rsp(entityID, sceneID, posX, posY, posZ)
    massive_send_to_client(sessionID, MSG_LOGIN_ENTER_WORLD_RSP, rspData)  // ← 常量由 GenMsgBindings.py 自动生成

    massive_log_info("handle_enter_world: account={accountID} → entity={entityID}")
}

// ── 工具函数 ──
def extract_entity_id(fullEid : uint64) : uint32
{
    return uint32(fullEid & uint64(0xFFFFFFFF))
}
```

### 5.2 MoveIntent DECS 组件

```das
// Scripts/Components.das（追加）

[decs_template]
struct MoveIntent {
    targetPos : float3
    speed     : float
    sequence  : uint
    timestamp : uint64
    time      : float     // 服务器收到时间
}
```

## 6. 出站 Protobuf 构造 Bridge

每个需要脚本发送的消息类型对应一个 C++ Bridge 函数。Phase 3 手工实现以下函数：

```cpp
// 在 MassiveModule 中新增

// ── 6. 出站消息构造（Protobuf）──
addExtern<DAS_BIND_FUN(massive_build_enter_world_rsp)>(...);
//   array<uint8> massive_build_enter_world_rsp(uint64 entityID, uint32 sceneID,
//                                               float x, float y, float z)
//   → C++ 内部用 protobuf-lite 构造 EnterWorldRsp，返回序列化字节

addExtern<DAS_BIND_FUN(massive_build_move_rsp)>(...);
//   array<uint8> massive_build_move_rsp(uint64 entityID, uint32 sequence,
//                                       float x, float y, float z)
//   → 同上，构造 MoveRsp
```

实现示例：

```cpp
das::TArray<uint8_t> massive_build_move_rsp(uint64_t entityID, uint32_t sequence,
                                            float x, float y, float z)
{
    Proto::MoveRsp rsp;
    rsp.set_sequence(sequence);
    rsp.mutable_position()->set_x(x);
    rsp.mutable_position()->set_y(y);
    rsp.mutable_position()->set_z(z);
    rsp.set_server_time(CurrentTimeMs());

    size_t size = static_cast<size_t>(rsp.ByteSizeLong());
    das::TArray<uint8_t> result(das::TypeDecl::getVectorType<uint8_t>(), size);
    rsp.SerializeToArray(result.data, static_cast<int>(size));
    result.size = static_cast<uint32_t>(size);
    return result;
}
```

## 7. Feature Flag 降级

```cpp
// WorldServer.h
bool _useScriptHandlers = true;  // 可通过 world.toml 配置

// 在 OnMessage 中：
if (_useScriptHandlers && msg.msgID >= kUserMsgIDStart)
    DispatchToScript(sessionID, msg);
else
    _dispatcher.Dispatch(sessionID, msg.msgID, msg.body.Data(), msg.body.Size());
```

**切换规则**：
- 生产环境默认 `_useScriptHandlers = true`
- 脚本出错时 GM 指令 `/togglescript off` 切回 C++ Handler
- 热重载失败时自动 fallback 到 C++ Handler

## 8. 验证流程

```
Phase 3 端到端验证：
  xmake up → LoginServer + GateServer + WorldServer 全部启动

  1. TestClient 连接 → 登录
  2. 验证：World 日志出现 "[script] handle_enter_world: account=X → entity=Y"
  3. 验证：客户端收到 LoginEnterWorldRsp → 进入游戏
  4. TestClient 发送移动包
  5. 验证：World 日志出现 "[script] handle_move: speed=X"
  6. 验证：客户端收到 MoveRsp → 位置正确
  7. GM 指令 /togglescript off → 切回 C++ Handler → 功能不中断
  8. GM 指令 /togglescript on → 切换到脚本 Handler → 功能恢复
```

## 9. 废弃清单

完成 Phase 3 验证后：

| 文件 | 处理 |
|------|------|
| `Src/World/Handler/EnterWorldHandler.cpp` | 保留——C++ fallback |
| `Src/World/Handler/EnterWorldHandler.h` | 保留——C++ fallback |
| `Src/World/Handler/MoveHandler.cpp` | 保留——C++ fallback |
| `Src/World/Handler/MoveHandler.h` | 保留——C++ fallback |

> C++ Handler 不删除——作为 feature flag 的 fallback。Phase 4 稳定运行 1 个月后评估是否可以删除。

## 11. `GenMsgBindings.py` —— 消息绑定代码生成器（Phase 2 即落地）

> 关联：[20_HandlerValidation](20_HandlerValidation.md) — `[msg_handler]` 编译期注解完整分析
> 不再推后到 Phase 4。Phase 2 与 MassiveModule 同步产出，xmake build 自动运行，**零手工 switch case**。

### 11.1 生成产物（4 个文件）

| 产物 | 作用 | 谁使用 |
|------|------|--------|
| `Src/World/AutoGen/MsgDispatch.gen.cpp` | C++ switch case — Protobuf 反序列化 → `das_invoke` | WorldServer 编译链 |
| `Src/World/AutoGen/MsgArgs.gen.h` | C++ DasLang struct 对应（`MoveReqArgs` 等） | `MsgDispatch.gen.cpp` include |
| `Scripts/AutoGen/HandlerRegistry.das` | DasLang 侧 Args struct + `[msg_handler]` 注解类 + 期望 msgID 列表 | Handlers.das require |
| `Scripts/AutoGen/MsgIDConstants.das` | `let MSG_MOVE_REQ = 200u` 等常量 | HandlerRegistry.das + Handlers.das require |

### 11.2 xmake 集成

```lua
-- Phase 2 生成规则——与 libDaScript 的依赖同步
rule("gen_msg_bindings")
    on_build(function (target)
        local genScript = path.join(os.projectdir(), "Tools/Script/GenMsgBindings.py")
        local pythonExe  = "python"  -- 或 os.getenv("PYTHON3")
        os.vrunv(pythonExe, {genScript,
            "--proto-dir", path.join(os.projectdir(), "Src/Proto"),
            "--cpp-out",   path.join(os.projectdir(), "Src/World/AutoGen"),
            "--das-out",   path.join(os.projectdir(), "Scripts/AutoGen")})
    end)

target("WorldServer")
    add_deps("libDaScript")       -- Phase 1 已加
    add_rules("gen_msg_bindings") -- ← Phase 2 新增
    add_files("AutoGen/*.gen.cpp")
```

### 11.3 操作流程（零手工 switch case）

```
1. 写好 .proto 文件（和以前一样）
2. xmake build
   └→ gen_msg_bindings rule 自动扫描新 proto → 产出 4 个文件
3. Handlers.das 中写 handler
   └→ [msg_handler(msg=MSG_NEW_REQ)]
       def handle_new(sessionID : uint32; var args : NewReqArgs) { ... }
```

**从未有过手写 switch case。** Phase 1 结束时还没有 Handler（只有 `init()` 和 `update()`），Phase 2 启动时生成器已就绪，第一个 handler 就是自动生成的。

### 11.4 与 C++ Handler fallback 的共存

生成器产物与 C++ fallback handler 独立共存：

```cpp
void WorldServer::OnMessage(uint32 sessionID, WorldSession &ws, const LogicMessage &msg)
{
    // 控制消息 → C++ 处理（不变）
    if (msg.msgID < kUserMsgIDStart) {
        OnControlMessage(msg.msgID, msg.body.Data(), msg.body.Size());
        return;
    }

    // 业务消息 → 自动生成的 dispatch（Phase 2 起）
    if (_useScriptHandlers) {
        DispatchToScript_AutoGen(sessionID, msg);  // ← MsgDispatch.gen.cpp
    } else {
        _dispatcher.Dispatch(sessionID, msg.msgID, msg.body.Data(), msg.body.Size());
    }
}
```

### 11.5 生成器脚本骨架

```python
# Tools/Script/GenMsgBindings.py — 扫描 .proto → 产出 4 个文件

import re, os, sys
from pathlib import Path

TYPE_MAP = {
    'float': 'float', 'double': 'float', 'int32': 'int', 'uint32': 'uint',
    'int64': 'int64', 'uint64': 'uint64', 'bool': 'bool', 'string': 'string',
    'bytes': 'array<uint8>',
}

def scan_proto(proto_path: str) -> list:
    """扫描 .proto → 返回 [{name, msgId, fields:[{name, type}]}]"""
    # 从 .proto 和 .pb.h 提取消息字段和 msgID
    pass

def gen_cpp_dispatch(messages: list) -> str:
    """生成 MsgDispatch.gen.cpp 的完整 switch case"""
    pass

def gen_cpp_args(messages: list) -> str:
    """生成 MsgArgs.gen.h 的 C++ struct"""
    pass

def gen_das_registry(messages: list) -> str:
    """生成 HandlerRegistry.das 的 Args struct + 注解 + 期望 msgID"""
    pass

def gen_msgid_constants(messages: list) -> str:
    """生成 MsgIDConstants.das 的 let MSG_XXX = NNNu 常量"""
    pass

if __name__ == '__main__':
    # parse argv --proto-dir --cpp-out --das-out
    # for each .proto in proto_dir/: scan → gen → write output files
    pass
```

> 生成器约为 200-250 行 Python。详细的字段提取、类型映射、C++ das_invoke 参数构造逻辑在实现时细化。
