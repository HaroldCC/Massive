# 基础设施 #8：GateServer — 无状态连接代理

> 状态：**设计确认**
> 关联：[s1_overview](../前期设计/architecture/s1_overview.html)（进程列表 §1.4）、[s2_network](../前期设计/architecture/s2_network.html)（网络层拓扑 §2.3）、[s3_queue](../前期设计/architecture/s3_queue.html)（MPSC 队列）、[s5_session](../前期设计/architecture/s5_session.html)（Session 生命周期 §5.1）、[s6_outbound](../前期设计/architecture/s6_outbound.html)（出站链路）、[s7_protocol](../前期设计/architecture/s7_protocol.html)（协议与加密 §7.3）、[01_Network.md](01_Network.md)（TCP 连接层）、[03_RPC.md](03_RPC.md)（内部 RPC 框架）、[07_LoginServer.md](07_LoginServer.md)（LoginServer）

## 1. 定位

GateServer 是 C2 拓扑中**唯一公网接入层**——无状态、零密钥、纯代理路由，吸收 DDoS。

| 属性 | 值 |
|------|-----|
| 公网暴露 | ✅ TCP（明文） |
| 进程数 | 2-8 |
| 最大连接数 | 10K+ / 实例 |
| 持有密钥 | **零**（无 LSS、无 SessionKey） |
| 共享状态 | **零**（可随意启停、水平扩展） |
| 连接拓扑 | 接受 Client TCP → 内部连接 WorldServer × N |

### 1.1 无状态意味着什么

GateServer 可以在任何时间重启、起任意数量实例，**不影响**登录/游戏业务：
- 重启时所有客户端断开连接 → WorldSession 进入 60s 等待重连期
- 新的 Gate 实例启动后，客户端用已有 SessionToken 重连即可
- 不需要与任何共享存储交互（无 DB、无 Redis、无共享内存）

### 1.2 加密边界

| 连接 | 加密 | Gate 的视角 |
|------|------|-------------|
| Client ↔ Gate | 明文（Body 密文） | 读 sessionId 路由，Body 原样透传 |
| Gate ↔ World（内网） | 明文（内网可信） | 封装 InternalHeader + 原包 |

> GateServer **不持有任何密钥**。Client↔World 的 AES-256-GCM 加密端到端应用层处理，Gate 不解密不解析——只做字节级别的转发。

## 2. 决策汇总

| 维度 | 选择 | 理由 |
|------|------|------|
| 帧协议（Client→Gate）| `PacketHeader` 模式（12B header）| 与现有 `PacketHeader.h` 一致，`TCPSocket(..., Framing::PacketHeader)` 直接复用 |
| 帧协议（Gate→World）| `LengthPrefix` 模式（4B 总长度）| 与 Center↔World 一致，`TCPSocket(..., Framing::LengthPrefix)` 直接复用 |
| GateSession 实现 | **包装 TCPSocket**，不自建 socket | 粘包拆包/异步写链/生命周期全部由 TCPSocket 提供，避免重复代码 |
| sessionId 分配 | `std::atomic<uint32>` 自增 | O(1)、简单、无需维护状态；重启归零概率碰撞可以忽略 |
| Gate→World 连接方向 | **Gate 主动连接 World** | Gate 知道 World 配置列表（从 gate.toml 读），主动建连；World 作为纯受方 |
| World 发现 | MVP：**静态配置**（gate.toml）；生产：**CenterServer 服务发现**（后补）|
| 断线重连 | **退避重连**（1s, 2s, 4s, ...→30s max）| 重连期间该 World 的客户端消息缓存，不断缓冲（有限上限）|
| 心跳归属 | **Gate 自回** | Client↔Gate 心跳只用于保活/超时断连，不转发到 World |
| 世界列表获取 | MVP：**静态列表**；Center 就绪后 → **RPC: PickWorld** | 跟 LoginServer 的 World 选择统一 |
| 限流/DDoS | **按 IP 连接数限流** + **全局连接上限** | 轻量级，不加复杂令牌桶 |

### 2.1（更正）Gate→World 内部协议——透传方案

**请以本节为准**，修正 s6_outbound 和 s2_network 中不一致的描述。

```
Gate→World 线格式（Framing::LengthPrefix，大端序）:

[TotalLength: 4B][InternalHeader: 4B][PacketHeader: 12B][EncryptedBody]

TotalLength    = 4 (自身) + 4 (InternalHeader) + 12 (PacketHeader) + bodyLen
InternalHeader = sessionID（Gate 分配，World 用此关联 WorldSession）
PacketHeader   = 客户端原始 PacketHeader（length+msgID+sessionID）
EncryptedBody  = 客户端原始 Body（AES-256-GCM 密文，Gate 零解析）
```

**为什么透传 PacketHeader？**
1. Gate 侧转发逻辑是 memcpy 级别简单——读完 InternalHeader，把剩余字节原样写 WorldConn
2. World 侧能从 PacketHeader 拿到 `msgID`（知道消息类型），从 `length` 校验完整性
3. 无需 Gate 侧维护任何 "消息类型白名单" 或 "字段重打包" 逻辑
4. 调试时 WireShark 抓包能在 Gate 侧看到完整的客户端帧

**World 侧入站解析**：
```
LengthPrefix 拆出 → InternalHeader.sessionID → 找 WorldSession
                   → PacketHeader.msgID → MessageDispatcher 分发
                   → EncryptedBody → CryptoSession.Decrypt() → 业务处理
```

**World→Gate 出站转发**：
```
WorldSession.Send(data):
  1. CryptoSession.Encrypt(body) → encryptedBody
  2. 构造 PacketHeader{length, msgID, sessionID}
  3. 构造 InternalHeader{sessionID}
  4. 组合: [InternalHeader(4B)][PacketHeader(12B)][encryptedBody]
  5. 前置 LengthPrefix(4B 总长度) → TCPSocket.Send()
```

**Gate 侧出站解析**：
```
WorldConn.OnMessage → 读 InternalHeader.sessionID
                    → 查 _sessionRoutes[sessionID] → clientSocket
                    → 写 TCP 去了 InternalHeader（客户端不认识这个头）
```

> ⚠️ **关键细节**：Gate→Client 出站时，必须**剥离 InternalHeader(4B)**。  
> 即：`[InternalHeader][PacketHeader][Body]` → Gate 剥离 InternalHeader → `[PacketHeader][Body]` → 写 Client。

剥离的方式：
- TCPSocket LengthPrefix 拆包后回调 `_onMessage(0, 0, data, len)`（`msgID=0, sessionID=0`）
- Gate 在 OnMessage 里自己解析前 4B 拿 sessionID，然后用 `TCPSocket::Send()` 把剩余部分写客户端
- Gate 的 `TCPSocket` 没有 Header 处理——这是 Pure IO 的逻辑，恰好符合

## 3. sessionId 分配

```cpp
// GateServer 内
std::atomic<uint32> _nextSessionID {1}; // 自增 ID，1 开始，0 保留为无效

uint32 AllocateSessionID()
{
    return _nextSessionID.fetch_add(1, std::memory_order_relaxed);
}
```

- 原子自增，O(1)，无需锁
- 0 值保留为无效 sessionId（`kInvalidSessionID = 0`）
- 重启归零：理论上重启后新连接的 ID 可能与旧连接残留在 World 侧的重连等待列表冲突——但概率极低且无害（World 侧会验证时间戳/Token，过期旧连接自然超时销毁）
- 安全边界：如果会话 ID 冲突，World 侧已有 `DisconnectedTag` 60s 超时机制保底

## 4. GateSession——包装 TCPSocket

> **决策**：GateSession **不自行管理 socket**，包装已有 `TCPSocket`。

```cpp
// GateSession.h — Gate 侧会话（极简无业务）
class GateSession : public std::enable_shared_from_this<GateSession>
{
public:
    explicit GateSession(uint32 sessionID, std::shared_ptr<TCPSocket> socket);

    // 生命周期
    void Start();               // 注册 TCPSocket 回调 → Start
    void Close();               // Gate 侧主动关闭

    // 写客户端的快捷方法
    void SendToClient(ByteBuffer data);

    // 访问器
    uint32 SessionID() const { return _sessionID; }
    uint16 WorldServerID() const { return _worldServerID; }
    void   SetWorldServerID(uint16 id) { _worldServerID = id; }

    bool   IsRouted() const { return _routed; }
    void   SetRouted() { _routed = true; }

    std::shared_ptr<TCPSocket>& Socket() { return _socket; }

private:
    uint32                     _sessionID;      // Gate 分配
    uint16                     _worldServerID = 0; // 从 SessionToken[0..1] 解析
    bool                       _routed = false; // 是否已完成 EnterWorld 路由
    std::shared_ptr<TCPSocket> _socket;          // 客户端 TCP 连接
};
```

```cpp
// GateSession.cpp
void GateSession::Start()
{
    auto self = shared_from_this();
    auto sockPtr = _socket.get();

    // 收到某个完整包时：
    //   1. 如果 !_routed → 拦截 MSG_LOGIN_ENTER_WORLD_REQ 处理路由
    //   2. 如果 _routed  → 转发到 World
    _socket->SetCloseHandler([self]() {
        // 通知 World：sessionId 断开
        self->_gateServer->OnSessionDisconnect(self->_sessionID);
        // 清理 sessionRoutes
        self->_gateServer->RemoveSession(self->_sessionID);
    });

    sockPtr->Start();
}
```

## 5. GateServer 主类

```cpp
// GateServer.h
class GateServer
{
public:
    bool Init(const GateConfig& cfg);
    void Run();
    void Stop();

private:
    // ── TCPAcceptor 回调 ──
    void OnNewClientConnection(std::shared_ptr<TCPSocket> socket);

    // ── 内部 TCP 连接管理 ──
    void ConnectToWorlds(const GateConfig& cfg);       // 启动时连所有 World
    void ReconnectToWorld(const std::string& addr);    // 退避重连

    // ── World 连接回调 ──
    void OnWorldMessage(const std::string& worldAddr,
                        const uint8* data, size_t len); // World→Client

    // ── 会话管理 ──
    void OnSessionEnterWorld(std::shared_ptr<GateSession> session,
                             const Proto::LoginEnterWorldReq& req);
    void OnSessionDisconnect(uint32 sessionID);
    void RemoveSession(uint32 sessionID);

    // ── 心跳 ──
    void OnHeartbeatReq(std::shared_ptr<GateSession> session,
                        const Proto::HeartbeatReq& req);
    void CheckClientTimeouts();  // 定时器

    // ── 组件 ──
    IOContextPool                                           _ioPool;
    TCPAcceptor                                             _acceptor;
    std::unordered_map<uint32, std::shared_ptr<GateSession>> _sessions; // sessionID→GateSession

    // Gate↔World 连接：地址 → WorldConnection
    struct WorldConnection
    {
        std::shared_ptr<TCPSocket> socket;
        bool                       connected = false;
        uint32                     reconnectDelay_ms = 1000;
    };
    std::unordered_map<std::string, std::unique_ptr<WorldConnection>> _worldConns;

    // 出站路由：sessionID → (worldAddr, clientSession)
    struct SessionRoute
    {
        std::string                           worldAddr;
        std::weak_ptr<GateSession>            clientSession;
    };
    std::unordered_map<uint32, SessionRoute> _sessionRoutes;

    // 路由缓存：worldAddr → 当前连在该 World 的 sessionId 列表（用于断线时批量通知）
    std::unordered_map<std::string, std::vector<uint32>> _worldSessionMap;

    // 限流/DDoS
    struct IPEntry { uint32 connCount; };
    std::unordered_map<std::string, IPEntry> _ipConnections;
    std::mutex                               _ipMutex;
    uint32                                   _totalConnections = 0;
    static constexpr uint32                  kMaxConnsPerIP   = 10;
    static constexpr uint32                  kMaxTotalConns   = 20000;

    std::atomic<uint32> _nextSessionID {1};
    GateConfig          _config;
};
```

### 5.1 消息分发逻辑

```
Client → Gate (PacketHeader 模式)

TCPSocket::ProcessReadBuffer() 解析 PacketHeader{length, msgID, sessionID}
  → 触发 _onMessage(msgID, sessionID, body, len)

Gate::OnClientMessage(socket, msgID, sessionID, body, len):
  |
  ├── msgID == MSG_HEARTBEAT_REQ
  |     → Gate 自回 MSG_HEARTBEAT_RSP（不转发 World）
  |     → 更新该 session 的最后活跃时间
  |
  ├── msgID == MSG_LOGIN_ENTER_WORLD_REQ  &&  !session->IsRouted()
  |     → 从 SessionToken[0..1] 读 worldServerId
  |     → 查 _worldConns → 选对应 World 连接
  |     → 构造：InternalHeader(sessionID) + PacketHeader(enterWorldReq) + Body
  |     → WorldConn.TCPSocket.Send()
  |     → _sessionRoutes[sessionID] = {worldAddr, session}
  |     → session->SetRouted()
  |
  ├── !session->IsRouted()
  |     → 丢弃（未进世界就发业务包）
  |
  ├── msgID 为其他业务消息 && session->IsRouted()
  |     → 查 _sessionRoutes[sessionID] → worldAddr
  |     → 构造：InternalHeader(sessionID) + 原始 [PacketHeader|Body](完整透传)
  |     → WorldConn.TCPSocket.Send()
  |
  └── _sessionRoutes 查不到
        → 丢弃/日志（不该发生）
```

### 5.2 出站分发逻辑

```
World → Gate (LengthPrefix 模式)

TCPSocket::ProcessLengthPrefixed() 拆出完整帧
  → 触发 _onMessage(0, 0, data, len)  [msgID/sessionID 均为 0]
  → Gate::OnWorldMessage(worldAddr, data, len):

  1. 解析 InternalHeader(4B):
     auto headerBuf = ByteBuffer::Wrap(data, 4);
     uint32 sessionID = headerBuf.ReadUint32();

  2. 查 _sessionRoutes[sessionID] → clientSession
     - 找到 → 把剩余部分 [PacketHeader | EncryptedBody] 写客户端
       auto clientBuf = ByteBuffer::Wrap(data + 4, len - 4);
       session->SendToClient(std::move(clientBuf));
     - 没找到 → 丢弃（客户端已断开）


  3. 控制消息（sessionID == 0）：走 RPC Notify 协议
     - 读 RPCHeader 解析 EInternalMsgID（如 MSG_DISCONNECT_NTF）
     - 投递到 Gate 内部的 RPCServerDispatcher 处理
     - 由 Gate 内部处理，不转发 Client

所有 World→Gate 控制消息统一走 RPC Notify 通道（而非特殊 InternalHeader 分支），
与 Center↔World 内部通信使用同一套协议栈。减少 World 侧消息分发分支。
```

### 5.3 GateSession 写客户端

```cpp
void GateSession::SendToClient(ByteBuffer data)
{
    // 直接调用 TCPSocket::Send（线程安全）
    // data 已包含 [PacketHeader | EncryptedBody]
    _socket->Send(std::move(data));
}
```

## 6. Gate↔World 连接管理

### 6.1 启动：主动连接所有 World

```cpp
void GateServer::ConnectToWorlds(const GateConfig& cfg)
{
    for (auto& addr : cfg.world.servers)
    {
        auto conn = std::make_unique<WorldConnection>();

        // 解析地址 "127.0.0.1:8001" → host + port
        auto colon = addr.find(':');
        std::string host = addr.substr(0, colon);
        uint16 port = static_cast<uint16>(std::stoi(addr.substr(colon + 1)));

        // asio 异步连接
        auto& ctx = _ioPool.GetNextContext();
        auto socket = std::make_shared<TCPSocket>(
            asio::ip::tcp::socket(ctx), Framing::LengthPrefix);

        socket->Socket().async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::address::from_string(host), port),
            [this, addr, socket](const asio::error_code& ec) {
                if (ec) {
                    Log::Error("Gate: connect to {} failed: {}", addr, ec.message());
                    ScheduleReconnect(addr);
                    return;
                }
                OnWorldConnected(addr, socket);
            });

        conn->socket = socket;
        _worldConns[addr] = std::move(conn);
    }
}
```

### 6.2 退避重连

```cpp
void GateServer::ReconnectToWorld(const std::string& addr)
{
    auto it = _worldConns.find(addr);
    if (it == _worldConns.end()) return;

    auto& conn = it->second;
    uint32 delay = conn->reconnectDelay_ms;
    conn->reconnectDelay_ms = std::min(delay * 2, 30000u);

    // 定时器退避重连（复用 TimingWheel 或 asio steady_timer）
    auto& ctx = _ioPool.GetNextContext();
    auto timer = std::make_shared<asio::steady_timer>(ctx, std::chrono::milliseconds(delay));
    timer->async_wait([this, addr, timer](const asio::error_code& ec) {
        if (ec) return;
        ConnectToWorldsWithAddr(addr); // 同上连接逻辑
    });
}
```

### 6.3 连接中断 → Session 处理

某 World 连接断开时：

1. 遍历 `_worldSessionMap[worldAddr]` 中所有 sessionId
2. 每个 sessionId 对应的 GateSession **标记为 "World 离线等待重连"**
3. 客户端消息继续接受**但不转发**（缓存在内存？还是直接丢弃？）
4. 重连成功后，Gate 给 World 发批量 `MSG_SESSION_REBIND_REQ` 通知所有 sessionId 回归

> MVP 简化：连接断开时直接断开所有该 World 的客户端——因为 60s 重连期由 WorldSession 处理，客户端重连时会连到其他 Gate 实例，并携带同一个 SessionToken。

## 7. 心跳与超时

### 7.1 Gate 自回心跳

```cpp
void GateServer::OnHeartbeatReq(std::shared_ptr<GateSession> session,
                                 const Proto::HeartbeatReq& req)
{
    // Gate 自回，不转发 World
    Proto::HeartbeatRsp rsp;
    rsp.set_server_time(GetSteadyClockMs());

    auto data = rsp.SerializeAsString();
    auto buf = ByteBuffer::Copy(
        reinterpret_cast<const uint8*>(data.data()), data.size());

    // 需要回包包含 PacketHeader
    auto packetBuf = BuildPacketHeader(Proto::MSG_HEARTBEAT_RSP,
                                        session->SessionID(), buf);
    session->SendToClient(std::move(packetBuf));

    // 更新会话最后活跃时间
    _sessionLastActive[session->SessionID()] = SteadyClock::now();
}
```

### 7.2 客户端超时断开

```
CheckClientTimeouts (每 10s 执行一次):
  for each session in _sessions:
    if now - session.lastActive > 60s:
      session.Close()
      RemoveSession(session.SessionID())
      // World 侧从 OnSessionDisconnect 收到 MSG_DISCONNECT
```

## 8. 限流 / DDoS 防护

### 8.1 连接级限流

```cpp
bool GateServer::AllowNewConnection(const std::string& clientIP)
{
    // 全局上限
    if (_totalConnections >= kMaxTotalConns) return false;

    // 单 IP 上限
    auto& entry = _ipConnections[clientIP]; // auto-create
    if (entry.connCount >= kMaxConnsPerIP) return false;

    entry.connCount++;
    _totalConnections++;
    return true;
}
```

- `kMaxConnsPerIP = 10`（一个普通玩家最多 1~2 个连接，10 足够）
- `kMaxTotalConns = 20000`（单实例，硬顶保护 OOM）
- 连接断开时 `_ipConnections[ip].connCount--`

### 8.2 全局反 DDoS

- **首次建立连接即分配 sessionId**，未认证的 session 没有到 World 的转发权（`!_routed`）
- 大量空连接只消耗 Gate 自身内存，不触及 World
- 如果 TCP 连接只发心跳不发 `MSG_LOGIN_ENTER_WORLD_REQ` → 60s 超时断开

> 正式 DDoS 防护（SYN Cookie、速率限制、IP 黑名单）通常在负载均衡器做，不在应用层。

## 9. GateConfig

```cpp
// GateConfig.h
struct GateConfig
{
    struct Network
    {
        uint16 port          = 9001;
        int    ioThreads     = 8;     // Gate 纯 IO，可多配
        uint32 maxConnections = 20000;
    } network;

    struct World
    {
        std::vector<std::string> servers; // WorldServer 内网地址列表
    } world;

    struct Heartbeat
    {
        uint32 clientTimeoutSec = 60;  // 客户端无心跳超时断连
    } heartbeat;

    Log::Config log;

    static std::optional<GateConfig> Load(const std::string& path);
};
```

```toml
# Config/gate.toml
[network]
port = 9001
io_threads = 8
max_connections = 20000

[world]
# WorldServer 内网地址列表（MVP 阶段静态配置）
servers = ["127.0.0.1:8001", "127.0.0.1:8002"]

[heartbeat]
client_timeout_sec = 60

[log]
level = 0
dir = "logs"
```

## 10. 控制消息 Proto

新增 `Src/Proto/Internal/GateRPC.proto`，走内部消息 ID 空间：

```protobuf
// GateRPC.proto — GateServer ↔ WorldServer 控制消息
syntax = "proto3";

package MMO.Proto.Internal;

// 客户端断线通知（World 侧进入 60s 重连等待）
message DisconnectNtf
{
    uint32 session_id = 1;
}

// 重连后，Gate 批量通知 World 哪些 sessionId 已恢复
message SessionRebindReq
{
    repeated uint32 session_ids = 1;
}

message SessionRebindRsp
{
    bool ok = 1;
}
```

对应 `InternalMsgID.proto` 追加：

```protobuf
enum EInternalMsgID
{
    // ... 已有 ...
    MSG_DISCONNECT_NTF = 11;
    MSG_SESSION_REBIND_REQ = 12;
    MSG_SESSION_REBIND_RSP = 13;
}
```

## 11. 线程模型

GateServer 为**纯 IO 进程**，无 LogicThread，无 DB 访问：

```
IOContextPool (N=8 线程)
  │
  ├── IO 线程 1: Client accept + Session_A + Session_B + ...
  │   ├── DoRead → ProcessReadBuffer → OnClientMessage (当前线程)
  │   │     ├── 心跳 → 直接回包
  │   │     ├── 路由 → WorldConn.TCPSocket.Send() (IO 线程安全)
  │   │     └── 业务 → 透传 InternalHeader + 原包 → WorldConn.Send()
  │   └── OnWorldMessage → 查 _sessionRoutes → Session.SendToClient (当前线程)
  │
  ├── IO 线程 2: Session_C + Session_D + ... (同上)
  │
  └── _sessionRoutes / _worldConns 访问：
        写（Add/Remove）→ 主线程 Init/Close 时加锁
        读（路由查表）  → 纯 IO 线程（无锁，只能读到本线程的 session）
```

> **GateServer 依赖 `TCPSocket::Send()` 的线程安全性**（内部有 `_writeMutex`），因此 `OnWorldMessage` 可以在任何 IO 线程中直接写 `session->Socket()->Send()`，无需跨线程投递。

## 12. 文件清单

```
Src/Gate/
├── main.cpp                   # 进程入口
├── GateServer.h / .cpp        # 主类：accept + 路由 + World 连接管理
├── GateSession.h / .cpp       # 会话封装（包装 TCPSocket + sessionId）
├── GateConfig.h / .cpp        # 强类型配置 (toml)

Config/
└── gate.toml                  # 配置

Src/Proto/
├── Gate.proto                 # (现有 Login.proto 已定义 LoginEnterWorldReq)
├── Internal/GateRPC.proto     # DisconnectNtf / SessionRebindReq

Src/Proto/Internal/
└── InternalMsgID.proto        # 追加 MSG_DISCONNECT_NTF 等
```

## 13. 依赖状态

| 依赖 | 基础设施 | 可用？ |
|------|----------|--------|
| TCPSocket / TCPAcceptor | #1 Network | ✅ 已有 |
| IOContextPool | #1 Network | ✅ 已有 |
| PacketHeader / ByteBuffer | #1 Network | ✅ 已有 |
| MessageDispatcher | #1 Network | ✅ 已有 |
| ConfigLoader / GateConfig | #2 Config | ✅ 已有（需新增 GateConfig） |
| SessionToken（路由读 worldServerId）| CommonCrypto | ✅ 已有 |
| toml++ | #2 Config | ✅ 已有 |
| Proto / MsgID | 已有 | ✅ 已有 |
| Internal RPC 帧（BuildRPCFrame）| #3 RPC | ✅ 已有 |
| TimingWheel（超时检查）| Timer | ✅ 已有 |
| Log | CommonLog | ✅ 已有 |

## 14. main.cpp 入口

```cpp
// main.cpp — GateServer 入口
int main(int argc, char* argv[])
{
    auto cfg = GateConfig::Load("Config/gate.toml");
    if (!cfg) return 1;

    Log::Init("gate", cfg->log);

    GateServer server;
    if (!server.Init(*cfg))
    {
        Log::Error("GateServer init failed");
        return 1;
    }

    server.Run();
    return 0;
}
```

```cpp
bool GateServer::Init(const GateConfig& cfg)
{
    _config = cfg;

    _ioPool = std::make_unique<IOContextPool>(
        static_cast<size_t>(cfg.network.ioThreads));

    // 监听客户端连接（PacketHeader 模式）
    _acceptor = std::make_unique<TCPAcceptor>(
        *_ioPool, cfg.network.port, Framing::PacketHeader);

    _acceptor->Start([this](std::shared_ptr<TCPSocket> socket) {
        // 连接级限流
        std::string clientIP;
        try {
            clientIP = socket->Socket().remote_endpoint().address().to_string();
        } catch (...) { clientIP = "0.0.0.0"; }

        if (!AllowNewConnection(clientIP)) {
            socket->Close();
            return;
        }

        uint32 sessionID = AllocateSessionID();
        auto session = std::make_shared<GateSession>(sessionID, std::move(socket));
        session->SetCloseHandler(...); // 断线通知
        // 绑定消息分发
        RegisterSessionHandlers(session);
        _sessions[sessionID] = session;
        session->Start();
    });

    // 内部：连接所有 WorldServer
    ConnectToWorlds(cfg);

    return true;
}

void GateServer::Run()
{
    _ioPool->Start();

    // 主线程：超时检查 + 世界连接重连检查
    while (_running)
    {
        CheckClientTimeouts();
        std::this_thread::sleep_for(1s);
    }
}
```

## 15. 未决/后续

- **CenterServer 服务发现**：MVP 后，Gate 从 CenterServer 的 `PickWorld` RPC 获取 World 地址列表，替换静态配置
- **TLS**：Client↔Gate TLS 加密；Gate 作为纯 IO 进程，TLS 在小包下 CPU 开销显著（~30%），生产环境通常在负载均衡器终结 TLS
- **批量 Disconnect 优化**：World 断线时批量通知所有 session，而非逐个发 `MSG_DISCONNECT_NTF`
- **连接统计 Prometheus**：Gate 对外暴露指标（在线连接数/转发延迟/World 连接状态）
- **Logout 消息桥接**：客户端主动断开时 Gate 应发送 `MSG_DISCONNECT_NTF`，让 World 立即保存 Entity，不等 60s 超时
