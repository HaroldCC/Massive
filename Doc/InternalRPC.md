# 内部通信协议设计（RPC）

> 状态：**已确认设计，待落地**
> 落地时机：CenterServer / SocialServer 开工时
> 关联文档：[s2_network](前期设计/architecture/s2_network.html)、[s4_logic_loop](前期设计/architecture/s4_logic_loop.html)、[s8_msgid](前期设计/architecture/s8_msgid.html)

## 1. 设计选型

内部通信采用**自研 proto + RPC 头**方案（业界游戏服务器主流之一，契合本项目裸 asio + protobuf + 单线程 LogicThread 的轻量栈）。

排除的方案：
- **Actor 模型**（skynet/Orleans/Akka）：与单线程 LogicThread + EnTT 模型冲突
- **gRPC**：引入 HTTP/2 和重依赖，破坏轻量裸 asio 设计；CenterServer 本身已承担服务发现职责，用不上 gRPC 的负载均衡那套重武器

## 2. 与客户端协议完全独立

| 维度 | 客户端协议 | 内部协议 |
|------|-----------|---------|
| 目录 | `Src/Proto/*.proto` | `Src/Proto/Internal/*.proto` |
| MsgID enum | `EMsgID`（`MsgID.proto`） | `EInternalMsgID`（`Internal/InternalMsgID.proto`） |
| 包头 | `PacketHeader`（length+msgID+sessionID） | `RPCHeader`（见下） |
| 加密 | AES-256-GCM | 明文（内网可信） |
| 通信语义 | 单向消息 | RPC（请求-响应配对） |
| 演进节奏 | 改了要更新客户端 | 改了只需重启相关服务端 |

两套 MsgID 各自维护"已有不变、新增追加、删除留废弃"，号段互不干扰。

## 3. 目录结构

```
Src/Proto/
├── Login.proto                  # 客户端协议
├── Move.proto                   # 客户端协议
├── MsgID.proto                  # 客户端 MsgID（git 追踪）
└── Internal/                    # 内部协议（独立体系）
    ├── CenterRPC.proto          # World↔Center: 注册/心跳/位置查询
    ├── SocialRPC.proto          # World↔Social: 公会/邮件/好友
    └── InternalMsgID.proto      # 内部 MsgID（独立 enum，git 追踪）
```

## 4. 内部包头

现有 `InternalHeader` 仅用于 Gate↔World 透传客户端消息（只含 sessionID，不变）。
RPC 新增独立帧头：

```cpp
#pragma pack(push, 1)
struct RPCHeader
{
    uint32 msgID;       // EInternalMsgID，大端
    uint64 requestID;   // 请求-响应关联：回包带相同 requestID
    uint64 traceID;     // 链路追踪（贯穿 Gate→World→Center）
    uint8  type;        // 0=Request, 1=Response, 2=Notify（无需回包）
    // 紧跟 protobuf body
};
#pragma pack(pop)
```

`requestID` 是 RPC 核心——发起方自增分配，响应方原样回填，发起方据此找到对应回调。

## 5. RPCClient（发起方，World 侧）

```cpp
class RPCClient
{
public:
    // 发起 RPC，回调在逻辑线程执行
    template <typename TReq, typename TRsp>
    void Call(ConnectionPtr conn, const TReq& req,
              std::function<void(const TRsp&)> onReply,
              std::chrono::milliseconds timeout = 5s);

    // Notify：单向，无需回包
    template <typename TMsg>
    void Notify(ConnectionPtr conn, const TMsg& msg);

    // 收到 RPC 响应时调用（按 requestID 找回调）
    void OnResponse(uint64 requestID, const uint8* body, size_t len);

    // 每 Tick 调用：检查超时请求，触发超时回调
    void ProcessTimeouts();

private:
    struct PendingCall {
        std::function<void(const uint8*, size_t)> callback;
        std::chrono::steady_clock::time_point deadline;
    };
    std::unordered_map<uint64, PendingCall> _pending;  // requestID → 回调
    std::atomic<uint64> _nextRequestID{1};
};
```

对接 [s4_logic_loop](前期设计/architecture/s4_logic_loop.html) §4.2 主循环的 `ProcessRPCResponses()` 和超时检查。

## 6. RPCServer（响应方，Center/Social 侧）

复用客户端的 `MessageDispatcher`，但 handler 签名带 `RPCContext`（能拿到 requestID 回包）：

```cpp
rpcServer.Register<QueryPlayerLocationReq>(
    MSG_INTERNAL_QUERY_LOCATION_REQ,
    [](RPCContext ctx, const QueryPlayerLocationReq& req) {
        QueryPlayerLocationRsp rsp;
        rsp.set_world_server_id(LookupLocation(req.account_id()));
        ctx.Reply(rsp);   // 自动回填 requestID
    });
```

## 7. 消息示例

```protobuf
// Internal/CenterRPC.proto
syntax = "proto3";
package MMO.Internal;

message RegisterWorldReq {
    uint32 world_server_id = 1;
    string internal_ip = 2;
    uint32 port = 3;
}
message RegisterWorldRsp {
    bool ok = 1;
}

message QueryPlayerLocationReq {
    uint32 account_id = 1;
}
message QueryPlayerLocationRsp {
    uint32 world_server_id = 1;  // 0 = 不在线
}
```

## 8. 命名约定

| 组件 | 命名 |
|------|------|
| 客户端 MsgID enum | `EMsgID` |
| 内部 MsgID enum | `EInternalMsgID` |
| RPC 帧头 | `RPCHeader` |
| RPC 发起方 | `RPCClient` |
| RPC 响应方 | `RPCServer` |
| RPC 上下文 | `RPCContext` |

缩写 `RPC` 全大写（CodingStandard §1.3）。
