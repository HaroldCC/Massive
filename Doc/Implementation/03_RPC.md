# 基础设施 #3：内部 RPC 框架

> 状态：**设计已确认，待实现**
> 关联：[InternalRPC.md](../InternalRPC.md)（早期草案）、[s4_logic_loop](../前期设计/architecture/s4_logic_loop.html)（主循环预留 `ProcessRPCResponses()`）、[center_server](../前期设计/architecture/center_server.html)（Center RPC 方法）

## 1. 目标

为内部进程间通信提供请求-响应（RPC）语义：World↔Center、World↔Social。

核心保证：**"一定有结果"**——成功回调/超时回调/断线回调，绝不永久挂起。

## 2. 决策汇总

| 维度 | 选择 | 理由 |
|------|------|------|
| RPC 关联 | `RPCHeader.requestID` 自增 + 响应回填 | 自研轻量方案，无 gRPC 依赖 |
| 回调线程 | World: LogicThread（MPSCQueue 投递）；Center/Social: IO 线程直跑 | Center 是纯 IO 进程无 LogicThread |
| 超时 | 超时即失败回调，不自动重试 | 重试由业务层决定 |
| 断线 | 在途立即失败 + 退出重读 | 不能永久挂起 |
| 序列化 | 复用 protobuf | 与客户端协议一致 |
| handler 注册 | 复用 `MessageDispatcher<RPCContext>` | 复用现有设施 |
| API 风格 | 回调式（成功 + 失败）| 与 DBWorkerPool 一致 |

## 3. RPCHeader

`RPCHeader` 位于 `Src/Common/Network/RPCHeader.h`：

```cpp
#pragma pack(push, 1)
struct RPCHeader
{
    uint32 msgID;       // EInternalMsgID，大端
    uint64 requestID;   // 请求-响应关联
    uint64 traceID;     // 链路追踪
    uint8  type;        // 0=Request, 1=Response, 2=Notify
};
#pragma pack(pop)
```

- `requestID`: 发起方自增分配，响应方原样回填，发起方据此找到回调
- `type=Notify`: 单向通知，无需回包（如心跳、上下线通知）
- Wire 上 `RPCHeader + protobuf body`

## 4. RPCContext（handler 签名）

定位：`Src/Common/Network/RPCContext.h`。

```cpp
struct RPCContext
{
    uint64                 requestID;
    std::shared_ptr<class TCPSocket> socket;

    // 回包：序列化 TMsg → RPCHeader(type=Response, requestID) + body → Send
    template <typename TMsg>
    void Reply(const TMsg& msg);
};
```

Center/Social 的 handler 签名：
```cpp
[](RPCContext ctx, const QueryPlayerLocationReq& req) {
    QueryPlayerLocationRsp rsp;
    // ... 查询 ...
    ctx.Reply(rsp);
}
```

## 5. RPCClient（发起方）

定位：`Src/Common/Network/RPCClient.h/.cpp`。

```cpp
class RPCClient
{
public:
    // 发起 RPC
    // onReply: 成功回调（LogicThread 执行）
    // onError: 失败回调——超时/断线/序列化失败（LogicThread 执行，可选）
    template <typename TReq, typename TRsp>
    void Call(std::shared_ptr<TCPSocket> conn,
              const TReq& req,
              std::function<void(const TRsp&)> onReply,
              std::function<void()> onError = nullptr,
              std::chrono::milliseconds timeout = 5000ms);

    // 单向通知（无回包）
    template <typename TMsg>
    void Notify(std::shared_ptr<TCPSocket> conn, const TMsg& msg);

    // 收到 RPC 响应 → 匹配 requestID → 投递回调到 LogicThread
    void OnResponse(uint64 requestID, const uint8* body, size_t len);

    // 连接断开 → 所有在途请求触发 onError
    void OnConnectionLost();

    // 每 Tick 检查超时（LogicThread 调用）
    void ProcessTimeouts();

private:
    struct PendingCall
    {
        std::function<void(const uint8*, size_t)> onReply;     // 成功
        std::function<void()>                      onError;     // 失败
        std::chrono::steady_clock::time_point      deadline;
    };
    std::unordered_map<uint64, PendingCall> _pending;
    std::atomic<uint64> _nextRequestID{1};
};
```

### 5.1 线程模型

WorldServer 侧（LogicThread 进程）：
```
IO 线程收到 RPC 回包 → RPCClient.OnResponse(requestID, body, len)
  → 匹配 _pending[requestID]
  → 封装 DBCallback{onReply, deserializedMsg}  → 塞入 _completedQueue(MPSCQueue)
LogicThread::ProcessRPCResponses() → DrainAll → 执行回调（无锁 ECS 操作）
```

Center/Social 侧（纯 IO 进程）：
```
IO 线程收到回包 → RPCClient.OnResponse() → 直接执行回调
  → 操作 ServiceRegistry / PlayerLocationIndex（单 IO 线程已线程安全）
```

### 5.2 超时处理

```cpp
void RPCClient::ProcessTimeouts()
{
    auto now = SteadyClock::now();
    for (auto it = _pending.begin(); it != _pending.end(); ) {
        if (now > it->second.deadline) {
            if (it->second.onError) {
                EnqueueCallback([cb = std::move(it->second.onError)] { cb(); });
            }
            it = _pending.erase(it);
        } else ++it;
    }
}
```

### 5.3 连接断开

```cpp
void RPCClient::OnConnectionLost()
{
    for (auto& [id, call] : _pending) {
        if (call.onError) {
            EnqueueCallback([cb = std::move(call.onError)] { cb(); });
        }
    }
    _pending.clear();
}
```

### 5.4 序列化流程

```
Call<Req, Rsp>(conn, req, onReply, onError, timeout):
  1. 序列化 req → body (protobuf)
  2. requestID = _nextRequestID++
  3. _pending[requestID] = { onReply, onError, now+timeout }
  4. RPCHeader{msgID, requestID, 0, Request} + body → conn->Send()
```

```
OnResponse(requestID, body, len):
  1. 查 _pending[requestID]
  2. RPCHeader.type == Response → 按 TRsp 反序列化 body
  3. 投递 onReply(deserializedRsp) 到 LogicThread
  4. 不复存在 → 丢弃（已超时/断线已移除）
```

## 6. RPCServer（响应方，handler 注册）

Center/Social 的 handler 注册复用 `MessageDispatcher<RPCContext>`。

```cpp
// Center 侧
MessageDispatcher<RPCContext> _rpcHandlers;

_rpcHandlers.Register<QueryPlayerLocationReq>(
    MSG_INTERNAL_QUERY_LOCATION_REQ,
    [](RPCContext ctx, const QueryPlayerLocationReq& req) {
        QueryPlayerLocationRsp rsp;
        rsp.set_world_server_id(Lookup(req.account_id()));
        ctx.Reply(rsp);
    });

// 收包 → 分发
void OnRPCPacket(const RPCHeader& header, const uint8* body, size_t len)
{
    RPCContext ctx{header.requestID, _socket};
    if (header.type == Request) {
        _rpcHandlers.Dispatch(ctx, header.msgID, body, len);
    }
}
```

## 7. 连接管理（CenterClient）

World 到 Center 的长连接需要：
1. 建立连接（`TCPSocket`）
2. 发送 `REGISTER` → 收到 `REGISTER_RSP` → 开始心跳
3. 断线 → 自动重连（带退避 `1s, 2s, 4s, ... → max 30s`）
4. 重连后重新 REGISTER

```cpp
// World 侧
class CenterClient
{
    void Connect(const std::string& host, uint16 port);
    void SendHeartbeat(uint32 currentPlayers);
    void SendRPC(...);                 // 通过 RPCClient 发起

    std::shared_ptr<TCPSocket> _socket;
    RPCClient                  _rpcClient;
    bool                       _registered = false;
};
```

> 注：重连逻辑作为后续增强，本期先假设连接稳定。RPCClient 的 `OnConnectionLost` 已预留接口。

## 8. 与 MsgID 生成器的衔接

内部协议走独立的 `EInternalMsgID`（`Src/Proto/Internal/InternalMsgID.proto`）。
`GenMsgID.py` 对 `Src/Proto/Internal/` 单独扫一次，规则不变（后缀识别 + 已有不变 + 新增追加）。

## 9. 对主循环的对接

回顾 [s4_logic_loop](Doc/前期设计/architecture/s4_logic_loop.html) §4.2 的核心循环：

```cpp
void LogicServer::Run()
{
    while (!_stopped.load()) {
        ProcessMessages();           // Drain Gate 队列 + MsgID 查表分发
        _timingWheel.Tick();
        ProcessDBWorkerCallbacks();  // DBWorkerPool 的结果回调
        ProcessRPCResponses();       // ← RPCClient 的回调
        Tick(elapsed);
        FlushOutgoingMessages();
    }
}
```

`ProcessRPCResponses()` 的实现：
```cpp
void WorldServer::ProcessRPCResponses()
{
    _rpcClient.ProcessTimeouts();    // 超时检查
    // Drain _completedQueue (MPSCQueue) → 执行成功/失败回调
    std::vector<RPCCompleted> batch;
    _rpcCompletedQueue.DrainAll(batch);
    for (auto& cb : batch) cb();
}
```

## 10. 文件清单

```
Src/Common/Network/
├── RPCHeader.h             # 内部 RPC 帧头
├── RPCContext.h            # handler 回包上下文
├── RPCClient.h / .cpp      # RPC 发起方
├── RPCServerDispatcher.h   # RPC handler 注册（MessageDispatcher<RPCContext> 别名）
├── CenterClient.h/.cpp       # World→Center 长连接客户端
```

## 11. 未决/后续

- **连接重连**：本期先假设连接稳定，CenterConnection 做后增强。
- **RPC 统计**：延迟直方图、超时率（运营期加 Prometheus 指标）。
- **单向通知的可靠性**：Notify 丢失怎么办？等 Center 做业务时定策略。
- **流量控制**：World→Social 批量 RPC 的并发限制（等 Social 开工时定）。
