# 基础设施 #1：TCP 连接层（Network）

> 状态：**设计已确认，待实现**
> 关联：[s2_network](../前期设计/architecture/s2_network.html)、[s5_session](../前期设计/architecture/s5_session.html)、[s7_protocol](../前期设计/architecture/s7_protocol.html)
> 参考：TrinityCore `shared/Networking/`（Socket / AsyncAcceptor / SocketMgr 分层）

## 1. 目标

`CommonNetwork` 目前只有 `IOContextPool`（线程池），缺少：
- 传输层 Socket 封装（异步收发、生命周期）
- 接受器（监听端口、accept 新连接）
- 粘包拆包（TCP 字节流 → 按 `PacketHeader.length` 切完整包）

本模块补齐这三块，作为所有进程（Login/Gate/World/Center/Social）的网络基座。

## 2. 命名与分层（参照 TrinityCore）

TrinityCore 的网络层分层验证了"传输层 Socket + 应用层 Session"的设计：

| 层 | TrinityCore | 本项目 | 职责 |
|----|-------------|--------|------|
| 传输基类 | `Socket<T>` (CRTP) | `ISocket`（未来提取）| Send/Close + OnMessage 回调 |
| TCP 实现 | `WorldSocket` | **`TCPSocket`** | TCP 收发 + 粘包拆包 |
| KCP 实现 | — | **`KCPSocket`**（未来）| UDP 上的可靠传输，低延迟 |
| 接受器 | `AsyncAcceptor` | **`TCPAcceptor`** | 异步 accept |
| 会话 | `WorldSession` | **`GateSession`/`WorldSession`** | 应用层会话状态，持有 Socket |

**命名规范**：缩写全大写（`TCP`/`UDP`/`KCP`，非 `Tcp`），符合 CodingStandard §1.3。

**分层原则**：
- `TCPSocket` 是"管道"——纯粹收发字节 + 拆包，不含业务
- `GateSession`/`WorldSession` 是"会话"——持有一个 Socket，加 sessionID + 业务状态
- Session 生命周期可跨 Socket（断线重连保留 session，见 s5_session §5.2）

## 3. 多协议策略：预留 KCP，先实现 TCP

### 决策

**现在只实现 `TCPSocket`，按 `ISocket` 接口形状编写，但暂不提取虚基类。**

理由（YAGNI + 预留扩展点）：
- 文档现有拓扑全是 TCP（s2_network §2.3），KCP 短期不用
- 现在抽 `ISocket` 虚接口 + 三种实现 = 维护用不上的代码
- 但按"未来能抽 ISocket"的接口形状写（`Send(ByteBuffer)` / `Close()` / `OnMessage` 回调），未来加 KCP 只需机械重构
- 不引入虚函数，保持 TCP 热路径性能

### 协议用途规划（未来）

| 协议 | 用途 | 可靠性 |
|------|------|--------|
| TCP | 登录、聊天、交易、背包 | 可靠有序 |
| KCP | 战斗、移动、技能（低延迟）| 可靠，弱网比 TCP 快 30-40% |
| UDP | （暂无直接用途）| 不可靠 |

### 扩展点

未来做战斗系统、确认需要 KCP 时：
1. 提取 `ISocket` 虚接口（`TCPSocket` 已是这个形状）
2. 新增 `KCPSocket : ISocket`
3. 上层 Session 持有 `ISocket*`，运行时多态
4. 一个进程可同时监听 TCP + KCP 端口（如 World 业务消息走 KCP，登录走 TCP）

## 4. 核心类设计

### 4.1 TCPSocket

```cpp
// 传输层 TCP socket，shared_ptr 生命周期 + 异步收发 + 粘包拆包
class TCPSocket : public std::enable_shared_from_this<TCPSocket>
{
public:
    // 回调（由上层 Session/Server 设置）
    using MessageHandler = std::function<void(uint32 msgID, uint32 sessionID,
                                              const uint8* body, size_t len)>;
    using CloseHandler   = std::function<void()>;

    explicit TCPSocket(asio::ip::tcp::socket socket);

    void Start();                  // 启动异步读循环
    void Send(ByteBuffer data);    // 异步写（线程安全，加锁写队列）
    void Close();

    void SetMessageHandler(MessageHandler h);
    void SetCloseHandler(CloseHandler h);

    asio::ip::tcp::socket& Socket();

private:
    void DoRead();                 // async_read_some → 累积到 _readBuffer
    void ProcessReadBuffer();      // 循环切出完整包（粘包处理）
    void DoWrite();                // 链式 async_write

    asio::ip::tcp::socket _socket;
    std::vector<uint8>    _readBuffer;   // 累积缓冲（批量读）
    std::mutex            _writeMutex;
    std::deque<ByteBuffer> _writeQueue;
    bool                  _writing = false;
    std::atomic<bool>     _closed{false};
    MessageHandler        _onMessage;
    CloseHandler          _onClose;
};
```

### 4.2 TCPAcceptor

```cpp
// 异步接受新连接，分配到 IOContextPool 的某个 io_context
class TCPAcceptor
{
public:
    using AcceptHandler = std::function<void(std::shared_ptr<TCPSocket>)>;

    TCPAcceptor(IOContextPool& pool, uint16 port);

    void Start(AcceptHandler onAccept);
    void Stop();

private:
    void DoAccept();

    IOContextPool&            _pool;
    asio::ip::tcp::acceptor   _acceptor;
    AcceptHandler             _onAccept;
};
```

## 5. 粘包拆包：缓冲 + 批量读

### 决策：方案 ii（缓冲累积 + 批量切包）

```
async_read_some(大缓冲) → 追加到 _readBuffer
  → 循环：
      若 _readBuffer 长度 >= sizeof(PacketHeader):
          解析 length（大端）
          若 _readBuffer 长度 >= length:
              切出 [0, length) 作为一个完整包 → 触发 OnMessage
              移除已消费字节
          否则 break（等更多数据）
      否则 break
```

理由：Gate 要扛 10K+ 连接 + 高频小包（移动包）。一次 `async_read_some` 切多个包，
摊薄 syscall 开销，比"每包两次 async_read"（先读头再读体）吞吐高。

### 安全限制

- 单包最大长度限制（防恶意超大 length 打爆内存）：`kMaxPacketSize`
- `_readBuffer` 增长上限保护

## 6. 生命周期：shared_ptr 回调式

### 决策：方案 A（`enable_shared_from_this` + 回调）

参照 TrinityCore（`Socket<T> : enable_shared_from_this<T>`）和 asio 官方范式：
- 异步操作（async_read/async_write）的 completion handler 持有 `shared_from_this()`，
  保证操作进行中 Socket 不被析构
- 连接关闭 = 引用计数归零自动析构
- 上层用 `std::shared_ptr<TCPSocket>` 持有

不选协程（调试栈不直观）、不选句柄池（和 asio async 模型摩擦）——shared_ptr 在
万级连接下开销可接受，是 asio 最自然的用法。

## 7. 与现有模块的衔接

| 现有 | 衔接 |
|------|------|
| `IOContextPool` | `TCPAcceptor` 从 pool 取 io_context 分配给新连接（Round-Robin）|
| `PacketHeader` | 粘包拆包按 `PacketHeader.length` 切，解析 `msgID`/`sessionID` |
| `ByteBuffer` | Send 接收 ByteBuffer，读缓冲也用其封装 |
| `MessageDispatcher` | Socket 的 `OnMessage` 回调 → 上层调 Dispatcher.Dispatch |
| `Log` | 连接建立/断开/错误用 Log |

## 8. 文件清单（待实现）

```
Src/Common/Network/
├── TCPSocket.h / .cpp       # 传输层 TCP socket + 粘包
├── TCPAcceptor.h / .cpp     # 异步接受器
├── (IOContextPool.h/.cpp)   # 已有
├── (PacketHeader.h)         # 已有
├── (CryptoSession.h/.cpp)   # 已有
└── (MessageDispatcher.h)    # 已有
```

## 9. 未决/后续

- TLS：Client↔Login 需 TLS（s7_protocol §7.5）。asio `ssl::stream` 包装 TCPSocket，
  作为独立增强（`TLSSocket` 或 TCPSocket 加 SSL 模式），业务逻辑不变。本期先明文，TLS 后补。
- KCP：见第 3 节扩展点。
- 连接限流 / DDoS 防护（Gate 侧）：后续。
