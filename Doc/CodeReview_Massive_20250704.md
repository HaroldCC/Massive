# Massive MMO Server 代码评审报告

> 评审日期: 2026-07-04
> 评审范围: `src/` 全部核心代码 + `Doc/` + `Config/` + `xmake.lua`

---

## 总体评价

一个结构清晰、有想法的 C++23 MMO 游戏服务器框架。核心网络层、安全模型、ECS 架构设计合理，
参考 TrinityCore 的设计思路在很多地方是正确的。目前处于 MVP 中期阶段——核心链路（Login→Gate→World→Center）
已跑通，但不少子系统和生产级细节尚待完善。

**当前 LoC 估算**: ~6000-8000 行（不含 ThirdParty 和 AutoGen），5 个独立进程。

---

## 一、严重问题（必须修复）

### 1.1 CenterServer::_running 非原子，存在数据竞争

**位置**: `src/Center/CenterServer.h:76` + `CenterServer.cpp:74,78,86`

```cpp
// 头文件声明为普通 bool
bool _running = false;

// Run() 中读取
while (_running) { ... }

// Stop() 中写入（可能从信号处理线程调用）
_running = false;
```

`bool` 在 C++ 中不保证原子性。如果 `Run()` 循环在核心 0 运行、`Stop()` 从信号/SIGINT 处理
被调用，编译器可能将 `_running` 缓存在寄存器中导致 Run() 永不退出。

**修复**:
```cpp
std::atomic<bool> _running{false};  // 与其他 4 个 Server 类保持一致
```
另外 GateServer、WorldServer、LoginServer 都已正确使用 `std::atomic<bool> _running`，
CenterServer 是唯一遗漏的。

> **决策**: 改为 `std::atomic<bool>`。

---

### 1.2 SessionToken 字节序不一致 — WorldServerId 读取可能错误

**位置**: `src/Common/Crypto/SessionToken.h:38-43` + `src/Gate/GateServer.cpp:237`

GateServer 中手动大端解码:
```cpp
uint16 worldId = (static_cast<uint16>(tokenRaw[0]) << 8)
               | static_cast<uint16>(tokenRaw[1]);
```

但如果 LoginServer/WorldServer 调用了 `token.WorldServerId()`:
```cpp
uint16 WorldServerId() const {
    uint16 id;
    std::memcpy(&id, data, 2);  // 直接 memcpy = 主机字节序!
    return id;
}
```

`SessionTokenBuilder::Issue()` 写入时用的是哪个字节序需要确认。如果写的是大端（网络序），
则 `WorldServerId()` 在小端 x86 上会返回错误值。

**修复**: 明确约定 SessionToken 所有数值字段为大端序（网络序），`WorldServerId()`/`AccountId()`/
`ExpireTime()` 三个便捷读取函数内部做 `ntohs`/`ntohl` 转换。

> **决策**: 统一为大端序，便捷函数内部加字节序转换。

---

### 1.3 数据库密码硬编码提交

**位置**: `Config/world.toml:12`

```toml
conn_string = "host=127.0.0.1 port=5432 dbname=massive user=postgres password=cr11234"
```

> **决策**: 暂不修复。当前为本地开发环境，127.0.0.1 仅本机可访问，无暴露风险。
> 后续部署到公网前再从 toml 迁移到环境变量。

**未来修复方向**:
1. 轮换该密码
2. 从 toml 中移除密码，改用环境变量: `conn_string = "host=127.0.0.1 port=5432 dbname=massive user=postgres"`
3. 在 ConfigLoader 中添加 `GetEnv()` 支持，或使用 `${PGPASSWORD}` 环境变量

---

### 1.4 GateServer 五把互斥锁过度细化，复杂度无收益

**位置**: `src/Gate/GateServer.h:233-268` + 全部 `GateServer.cpp`

GateServer 维护了 5 把独立的互斥锁保护各自的数据:

| 锁 | 保护数据 | 访问频率（估算） |
|---|---|---|
| `_sessionsMutex` | `_sessions` (sessionID→GateSession) | 每条客户端消息 |
| `_routesMutex` | `_sessionRoutes` (sessionID→worldAddr) | 每条转发消息 |
| `_worldConnsMutex` | `_worldConns` (addr→socket) | 每条转发消息 |
| `_worldSessionMapMutex` | `_worldSessionMap` (world→session列表) | 仅 EnterWorld/断线 |
| `_ipMutex` | IP 计数器 | 仅连接/断开 |

**结论：当前代码不存在死锁**。每个函数内部的锁获取顺序一致，且所有锁都是 scoped
RAII，不存在跨函数嵌套持锁的情况。`CheckClientTimeouts` 的设计甚至明确考虑了此问题
（注释 "不能在持锁时 Close，会导致死锁"），正确地在持锁外收集超时 session 再 Close。

**真正的问题是设计过度复杂**，而非正确性缺陷：

1. **GateServer 是纯 IO 进程** — 所有回调在 asio IO 线程执行，无 LogicThread。
   这些数据结构的大小是数百到数千条目的 map，锁争用极低。5 把细粒度锁增加的认知
   负担远超其带来的理论并发收益。

2. **热路径上的双锁** — `ForwardToWorld` 是每条客户端消息的热路径，它先持
   `_routesMutex` 查找 worldAddr，释放后再持 `_worldConnsMutex` 查找 socket 发送。
   两次锁获取/释放在每条消息上。

3. **`_worldSessionMapMutex` 是冗余的** — `_worldSessionMap`（world→session列表）
   完全可以从 `_sessionRoutes` 推导。维护两份数据 + 独立锁 = 不必要的同步点。

4. **`_ipMutex` 可以独立** — 连接限流逻辑与其他数据无关，保留独立锁合理。

**修复方案**:

方案 A（推荐 — 合并为一把粗粒度锁）:
```cpp
// 合并 _sessionsMutex, _routesMutex, _worldConnsMutex, _worldSessionMapMutex
// 为一把 _gateMutex。GateServer 纯 IO 回调均短促，粗粒度锁不是瓶颈。
mutable std::mutex _gateMutex;

// _ipMutex 保持独立（限流逻辑无关）
std::mutex _ipMutex;
```
优点: 锁模型从 5 变 2，极大地降低心智负担；热路径从两次锁变一次。
缺点: 连接建立和路由查询互斥——但这两者在 GateServer 中都是亚毫秒级操作。

方案 B（保守 — 删除冗余锁，保留热路径锁）:
```cpp
// 删除 _worldSessionMapMutex，世界断线时从 _sessionRoutes 反查 session 列表
// 删除 _sessionsMutex 或将其与 _routesMutex 合并（两个总是成对出现）
// 保留 _worldConnsMutex + _routesMutex（合并后）+ _ipMutex
```

**建议选方案 A**。GateServer 的职责是"透传"，不是"并行计算"。10K 连接下每条消息
的锁持有时间仅数十纳秒，一把锁的吞吐完全够用。5 把锁的维护成本和未来的 bug 风险
远大于性能收益。

> **决策**: 选方案 A，合并为一把粗粒度锁。

---

## 二、中等问题（建议尽快修复）

### 2.1 SessionToken HMAC 仅 4 字节 — 碰撞抵抗力弱

**位置**: `src/Common/Crypto/SessionToken.h:29`

```cpp
static constexpr size_t kHmacSize = 4; // HMAC-SHA256 截断到 4 字节
```

4 字节 = 32 位 = 约 43 亿种可能。在线游戏场景下，攻击者可以暴力枚举伪造有效的 SessionToken。
虽然需要知道 LSS，但 4 字节的 HMAC 截断使得暴力碰撞的搜索空间缩小到了不安全的程度。

**修复**: 将 `kHmacSize` 增大到至少 8 字节（64 位）。Token 总大小从 46B 变为 50B，仍然紧凑。

> **决策**: `kHmacSize` 从 4 改为 8。

---

### 2.2 无任何单元测试

**位置**: 整个项目

`src/` 目录下无任何 test 文件，无测试框架集成。一个多进程分布式系统完全靠手工启动验证，
任何重构都是盲飞。

**修复方案**:

选用 **doctest**（header-only，与项目 vendored 依赖模式一致；xmake 原生支持 `add_tests()`）。

#### 2.2.1 引入 doctest

遵循项目现有 ThirdParty vendored 模式：

```
ThirdParty/doctest/doctest.h    ← 单头文件，从 https://github.com/doctest/doctest 获取
```

`ThirdParty/xmake.lua` 追加：

```lua
target("doctest")
    set_kind("headeronly")
    add_rules("Rules.ThirdParty")
    add_sysincludedirs("$(projectdir)/ThirdParty/doctest", {public = true})
```

#### 2.2.2 目录结构

```
Tests/
  main.cpp                        # #define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
  Common/
    TestByteBuffer.cpp
    TestMPSCQueue.cpp
    TestTimingWheel.cpp
    TestMessageDispatcher.cpp
  Crypto/
    TestSessionToken.cpp
    TestCryptoSession.cpp
```

#### 2.2.3 xmake 集成

```lua
-- Tests/xmake.lua
target("MassiveTests")
    set_kind("binary")
    set_group("Tests")
    add_files("Tests/**.cpp")
    add_deps("CommonCore", "CommonNetwork", "CommonCrypto", "CommonQueue",
             "CommonTimer", "CommonLog", "Proto", "doctest")
    add_includedirs("Src")

    -- 关键：注册为 xmake test 的默认测试 target
    add_tests("default")
```

根 `xmake.lua` 追加：`includes("Tests/xmake.lua")`

#### 2.2.4 运行方式

```bash
xmake build MassiveTests   # 仅构建
xmake test                 # 构建 + 运行所有 add_tests("default") 的 target
xmake run MassiveTests     # 手动运行看详细输出
```

#### 2.2.5 首批测试优先级

| 优先级 | 模块 | 理由 |
|--------|------|------|
| P0 | ByteBuffer | Own/Wrap/Copy 生命周期 + 字节序正确性，问题影响面最广 |
| P0 | MPSCQueue | 多生产者并发入队 + DrainAll 一致性 |
| P0 | SessionToken | Issue / Verify 往返，字节序验证 |
| P1 | TimingWheel | Schedule / Cancel / Tick / 级联降级 |
| P1 | CryptoSession | Encrypt / Decrypt 往返，序列号防重放 |
| P2 | MessageDispatcher | Register / Dispatch / 未注册 fallback |

---

### 2.3 异常处理过于宽泛 — catch(...) 吞没诊断信息

**位置**: 多处

```cpp
// GateServer.cpp:117-119
try {
    clientIP = socket->LowestLayer().remote_endpoint().address().to_string();
} catch (...) { }

// LoginServer.cpp:56-59 — 同样的模式
```

这些 `catch(...)` 在正常情况下不会触发，但一旦触发（如 asio 内部错误），日志中毫无痕迹。

**修复**: 捕获并记录 Error 级别日志，确保线上排查时有迹可循:
```cpp
try {
    clientIP = socket->LowestLayer().remote_endpoint().address().to_string();
} catch (const std::exception& e) {
    Log::Error("GateServer: remote_endpoint failed: {}", e.what());
} catch (...) {
    Log::Error("GateServer: remote_endpoint failed (unknown)");
}
```

---

### 2.4 LogicThread::ProcessMessages 迭代 sessions 时未持锁（理论 UB）

**位置**: `src/World/LogicThread.cpp:131` + `src/World/GateConnection.cpp:75-76`

IO 线程在 `GateConnectionMgr::OnGateMessage()` 中通过 shared_lock 持有 `_sessionsMtx`
后进行 `_sessions->find()`（读操作）。而 LogicThread 在 `ProcessMessages()` 中直接
`for (auto &[sid, ws] : *sessions)` 迭代整个 unordered_map，**未获取任何锁**。

C++ 标准规定同一 `std::unordered_map` 的并发 `find()` 和迭代（`begin()`/`end()`）
属未定义行为（[container.requirements.dataraces]），即使两者都是逻辑上的"读取"。

当前在所有主流 stdlib 实现（libstdc++/libc++/MSVC STL）上碰巧可运行，但严格依赖
未定义行为，未来编译器升级/优化可能引入难以调试的 heisenbug。

**修复**:
```cpp
void LogicThread::ProcessMessages(...) {
    if (!sessions || !sessionsMtx) return;
    std::shared_lock lock(*sessionsMtx);  // 与 IO 线程的 shared_lock 兼容
    for (auto &[sessionID, ws] : *sessions) { ... }
}
```

同理，`OnTick → ProcessUnroutedMessages` 中写 `_sessions`（insert/erase）时，
需确保与 IO 线程的 shared_lock 互斥——当前也未加锁。

---

### 2.5 EnterWorldHandler 重连逻辑的 sessionID 冲突

**位置**: `src/World/Handler/EnterWorldHandler.cpp:130-175`

```cpp
auto oldEntry = sessions.extract(oldSessionID);
auto &ws = oldEntry.mapped();
ws.sessionID = sessionID;  // 更新为新的 sessionID
sessions[sessionID] = std::move(ws);
```

旧 sessionID 的节点被 extract，新 sessionID 被 insert。但如果新 sessionID 恰好
等于旧 sessionID（Gate 复用），`sessions[sessionID] = std::move(ws)` 等价于原地赋值，
无问题。但如果不同，且 `sessions` 中已存在 `sessionID`（极少但非不可能），会静默覆盖。

**修复**: 加断言或检查:
```cpp
if (sessions.contains(sessionID)) {
    Log::Error("EnterWorld: sessionID {} already in use during reconnect!", sessionID);
    SendError(sessionID, 2004, "Internal error", gateSendFn);
    return;
}
```

> **决策**: 选此方案。sessionID 碰撞说明 Gate/World 生命周期管理有 bug，暴露它比掩盖好。

---

## 三、改进建议（非紧急但值得投入）

### 3.1 ByteBuffer 字节序转换优化

**位置**: `src/Common/Core/ByteBuffer.h:107-232`

当前手动逐字节移位实现大端/主机序转换。现代编译器能识别此模式并自动优化为 `bswap`，
实际性能已最优，但代码可读性差。

项目使用 C++23，可直接用标准库 `std::byteswap`：

```cpp
#include <bit>   // C++20: std::endian; C++23: std::byteswap
uint32 ReadUint32() {
    CheckRead(4);
    uint32 result;
    std::memcpy(&result, _data + _readPos, 4);
    _readPos += 4;
    if constexpr (std::endian::native == std::endian::little)
        result = std::byteswap(result);
    return result;
}
```

编译器产出与当前手写移位相同的 `bswap` 指令，零性能损失，可读性大幅提升。

> **决策**: 用 `std::byteswap` 替换手动移位。

---

### 3.2 客户端消息发送：模板膨胀 + SerializeAsString 性能浪费（联合修复）

**位置**: 多个文件（见下）

两个独立但相关的问题，建议一起修复。

---

#### 问题 A：SendToClient 模板导致代码膨胀

`src/World/WorldServer.h:93-123` — `SendToClient<TMsg>` 模板定义在头文件，
100+ 消息类型各实例化一份加密/组帧/发送逻辑，增加编译时间和二进制体积
（但实际只有 `WorldServer.cpp` 包含它，当前不扩散）。

**根因**：模板中类型相关的只有第一行 `msg.SerializeAsString()`，
后序加密、组帧、发送全部类型无关。

---

#### 问题 B：SerializeAsString() 全局误用

项目中 RPC 层（`RPCFrame.h` / `RPCContext.h`）已正确使用
**`ByteSizeLong()` + `SerializeToArray()`** 零分配序列化模式，注释明确标注了优化意图。
但客户端消息路径全部使用 `SerializeAsString()` → 再 `ByteBuffer::Copy` 拷贝，
每条消息导致两次不必要的堆分配 + 一次 memcpy。

受影响的文件和函数：

| 文件 | 函数 | 调用模式 |
|------|------|---------|
| `World/WorldServer.h:96` | `SendToClient<TMsg>` | `SerializeAsString()` → `ByteBuffer::Copy()` |
| `Gate/GateServer.cpp:298` | `SendEnterWorldError` | `SerializeAsString()` → 手动 build PacketHeader |
| `Gate/GateServer.cpp:329` | `OnHeartbeatReq` | `SerializeAsString()` → 手动 build PacketHeader |
| `Login/LoginServer.cpp:293` | `SendAuthSuccess` | `SerializeAsString()` → 手动 build PacketHeader |
| `Login/LoginServer.cpp:332` | `SendAuthFailure` | `SerializeAsString()` → 手动 build PacketHeader |
| `World/Handler/EnterWorldHandler.cpp:183` | `SendError` | `SerializeAsString()` → `ByteBuffer::Copy()` |
| `World/Handler/EnterWorldHandler.cpp:205` | `SendRsp` | `SerializeAsString()` → `ByteBuffer::Copy()` |
| `World/Handler/MoveHandler.cpp:57` | MoveRsp 发送 | 走 SendToClient → 间接受影响 |
| `TestClient/VirtualClient.cpp` (4处) | 测试发包 | 同上，非关键 |

**ParseFromArray 不受影响** — 收包路径只能从已接收的字节数组解析，无优化空间。

---

#### 已有正确示例（RPC 层）

`src/Common/Network/RPCFrame.h` 已实现最优模式：

```cpp
template <typename TMsg>
ByteBuffer BuildRPCFrame(uint32 msgID, uint64 requestID, ERPCType type,
                         uint64 traceID, const TMsg &msg) {
    size_t bodySize = msg.ByteSizeLong();           // Pass 1: 计算大小
    size_t total    = sizeof(RPCHeader) + bodySize;
    auto   buf      = ByteBuffer::Own(total);        // 分配 1: 帧 buffer

    buf.WriteUint32(msgID);                          // 写 header
    buf.WriteUint64(requestID);
    buf.WriteUint64(traceID);
    buf.WriteUint8(static_cast<uint8>(type));

    msg.SerializeToArray(buf.WritePtr(), bodySize);  // Pass 2: 直接写入
    buf.SetWritePos(buf.WritePos() + bodySize);      // 手动推进写位置

    return buf;
}
```

---

#### 联合修复方案

**方案：`MessageLite` 消灭模板 + 统一序列化路径**

```cpp
// ===== WorldServer.h — 不再是模板 =====
void SendToClient(uint32 sessionID, uint32 msgID,
                  const google::protobuf::MessageLite &msg);

// ===== WorldServer.cpp =====
void WorldServer::SendToClient(uint32 sessionID, uint32 msgID,
                                const google::protobuf::MessageLite &msg) {
    auto it = _sessions.find(sessionID);
    if (it == _sessions.end()) {
        Log::Warn("SendToClient: session {} not found", sessionID);
        return;
    }

    // 一步到位: protobuf → ByteBuffer，零中间分配
    size_t bodySize = static_cast<size_t>(msg.ByteSizeLong());
    auto   bodyBuf  = ByteBuffer::Own(bodySize);
    msg.SerializeToArray(bodyBuf.WritePtr(), static_cast<int>(bodySize));
    bodyBuf.SetWritePos(bodySize);

    auto encrypted = it->second.crypto.Encrypt(bodyBuf.Data(), bodyBuf.Size());
    if (encrypted.Size() == 0) return;

    uint32 totalLen = static_cast<uint32>(sizeof(PacketHeader) + encrypted.Size());
    auto   frame    = ByteBuffer::Own(totalLen);
    frame.WriteUint32(totalLen);
    frame.WriteUint32(msgID);
    frame.WriteUint32(sessionID);
    frame.WriteBytes(encrypted.Data(), encrypted.Size());

    _gateConnMgr->SendToGate(it->second.gateServerID, sessionID, std::move(frame));
}
```

```cpp
// ===== GateServer / LoginServer: 同样改用 MessageLite + 零分配序列化 =====
void SendClientPacket(std::shared_ptr<TCPSocket> socket,
                      uint32 msgID, uint32 sessionID,
                      const google::protobuf::MessageLite &msg);
// 实现: ByteSizeLong → ByteBuffer::Own → SerializeToArray → SetWritePos → Send
```

**收益汇总**：

| 指标 | 当前 | 修复后 |
|------|------|--------|
| SendToClient 模板实例化 | 每消息类型 1 份 | **0 份**（非模板） |
| 每条消息堆分配 | 4 次 | **2 次** |
| 每条消息 memcpy | 1 次 | **0 次** |
| 调用方代码变化 | 无需改动（`MessageLite` 隐式转换） | — |

**风险评估**：`MessageLite` 的虚函数 dispatch（~5ns）在 AES-GCM 加密（~2µs）和
asio 异步发送（~5µs+）面前完全可忽略。所有 `.pb.h` 生成类的基类链均包含
`MessageLite`，接口兼容性无问题。

---

### 3.3 添加 README.md

项目根目录无 README。新贡献者甚至不知道这是什么项目。

> **决策**: 暂不添加。当前项目为个人开发，待需要外部协作时再补。

**建议内容**: 一句话介绍 + 架构图 + 快速构建步骤 + Doc/ 导航。

---

### 3.4 SocialServer 是空壳但参与构建

**位置**: `src/Social/xmake.lua`

```lua
target("SocialServer")
    set_kind("binary")
    set_default(false)  -- 不默认构建
    add_deps("CommonCore", "CommonDB", "CommonNetwork", "CommonLog", "Proto")
```

`set_default(false)` 意味着不会拖慢日常构建，但该目录只有一个 xmake.lua，无任何源文件。
如果 target 没有 .cpp 源文件，构建时可能产生空 exe 或链接错误。

> **决策**: 暂不修复。SocialServer 后续开发时会自然补全源文件。

**修复方向**: 添加 TODO 注释或占位 main.cpp，或暂时从根 xmake.lua 的 includes 中移除。

---

### 3.5 WorldServer::SendToClient 中查找 session 未加锁

**位置**: `src/World/WorldServer.h:99-104`

```cpp
auto it = _sessions.find(sessionID);
if (it == _sessions.end()) { ... return; }
```

`SendToClient` 被 `_dispatcher.Dispatch` 从 LogicThread 调用，而 LogicThread 独占
sessions 的写权限，所以当前是安全的。但函数声明为 public，如果未来从 IO 线程调用
（如 GateConnection 回调），就会 data race。

**修复**: 添加注释明确线程约束:
```cpp
/// @warning 必须在 LogicThread 中调用（独占 _sessions）
```

> **决策**: 加注释即可。3.2 实施后 SendToClient 移入 `.cpp`，外部误调用风险自然消除。

---

### 3.6 分级配置验证

**位置**: 所有 `*Config::Load()` 函数

配置加载使用 "缺失则取默认值" 的策略。如果用户拼错了 toml key（如 `io_therads`），
会静默使用默认值 4，不会报错。

**修复**: 在 `ConfigLoader` 中加全局 flag，debug 模式下每次 fallback 到默认值时打 `Log::Warn`。

> **决策**: Debug 模式记录默认值即可，改动量 < 10 行。严格 schema 验证当前阶段维护成本过高。

---

### 3.7 定时器回调中的生命周期安全

**位置**: `src/World/WorldServer.cpp:383-386`

```cpp
_logicThread.GetTimingWheel().Schedule(std::chrono::seconds(30), [this, accountID]() {
    OnDisconnectTimeout(accountID);
});
```

定时器回调持有裸 `this` 指针。已验证 `LogicThread::Stop()` 先 join 逻辑线程（RunLoop 退出、
不再 Tick），再析构 `_timingWheel`（其析构函数遍历所有槽位 delete 全部 Node）。因此
WorldServer 析构时所有未决回调已被清理，`this` 不会成为野指针。

> **决策**: 已验证安全，无需修改。

---

### 3.8 代码格式化流程

> **决策**: 不处理。当前 `.clang-format` 已存在（项目根目录），`xmake format` 可用。

---

### 3.9 WorldConfig 与 LoginConfig hex decode 逻辑完全重复

**位置**: `World/WorldConfig.cpp:99-125` vs `Login/LoginConfig.cpp:24-59`

两个文件各自实现了完全相同的 `HexValue(char) -> int` 函数和 hex decode 循环：

```cpp
// 两份完全相同的代码：
auto hexVal = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
};
// + 相同的 for 循环逐字节 decode...
```

**修复**：提取到 `Common/Crypto/Hex.h`：

```cpp
// Common/Crypto/Hex.h
namespace MMO::Crypto {
    /// @return 成功解码的字节数，失败返回 0（含非法字符）
    size_t HexDecode(std::string_view hex, uint8* out, size_t outLen);
}
```

> **决策**: 提取共享函数，各删 ~30 行重复代码。

---

### 3.10 GateConnectionMgr::AcceptConnection 存在死代码

**位置**: `World/GateConnection.cpp:22`

```cpp
auto weakSock = std::weak_ptr<TCPSocket>(conn->socket);
```

`weakSock` 创建后从未被使用。MessageHandler 回调中直接捕获了 `this` 和 `gateID`，
不依赖 weak_ptr。

> **决策**: 删除该行。

---

### 3.11 CenterClient 同步阻塞连接（与 GateServer 风格不一致）

**位置**: `World/CenterClient.cpp:47`

```cpp
_socket->LowestLayer().connect(*endpoints.begin(), connectEc);
```

Init 阶段做同步 TCP connect，会阻塞调用线程直到握手完成。虽然 Init 时阻塞可接受，
但如果 CenterServer 不可达，OS 默认 TCP connect 超时可达 20-120 秒，WorldServer
启动会卡死在此期间。

**对比**：`GateServer::ConnectToWorld` 已正确使用 `async_connect` + 回调，CenterClient 应统一。

**修复**：改为异步连接，与 GateServer 的 `ConnectToWorld` 模式一致：

```cpp
_socket->LowestLayer().async_connect(*endpoints.begin(),
    [this, addr = host + ":" + std::to_string(port)](const asio::error_code& ec) {
        if (ec) {
            Log::Error("CenterClient: connect to {} failed: {}", addr, ec.message());
            return;
        }
        // 注册回调 + 发送 RegisterWorldReq...
    });
```

> **决策**: 改为 `async_connect`，与 GateServer 风格统一。

---

## 四、架构亮点（值得肯定）

### 4.1 多进程微服务拆分合理
Login/Gate/World/Center 职责边界清晰：
- **LoginServer**: 纯短连接认证，ECDH + argon2id + SessionToken 签发
- **GateServer**: 无状态连接代理，纯 IO 线程，零业务逻辑
- **WorldServer**: IO + LogicThread 分离，EnTT ECS + 加密通信
- **CenterServer**: 服务注册发现，RPC handler 模式

### 4.2 安全模型扎实
- 密码: argon2id (2 iters × 64 MiB, OWASP 2023 推荐参数)
- 密钥协商: X25519 ECDH
- 通信加密: AES-256-GCM (per-session key + sequence-based nonce)
- 重连安全: 密钥旋转 (HMAC-SHA256 派生新密钥)
- SessionToken: 自包含凭证，Gate 只读明文路由字段

### 4.3 性能设计意识
- MPSC 无锁队列 (moodycamel::ConcurrentQueue) 做 IO→Logic 通信
- 四级时间轮定时器 (O(1) Schedule/Cancel/Tick)
- 粘包批量处理: 一次 async_read_some 切多个包
- ByteBuffer Own/Wrap 双模式 (零拷贝解析 vs 拥有式构建)
- TCP/KCP 协议扩展预留

### 4.4 文档质量
`Doc/` 下有 45 个文件，设计文档覆盖网络、协议、ECS、部署、热更新等 20+ 主题。
文档引用了 TrinityCore 的成熟设计作为参考，非常务实。

### 4.5 开发体验
- `xmake up/down/logs` 一键起服/停服/看日志
- `xmake format` 统一代码风格
- `Tools/ServerCtl.py` 管理多进程生命周期
- ConfigLoader pImpl 隐藏 toml++ 依赖
- DB 代码自动生成 (GenDBBindings.py)

---

## 五、未完成项（按 doc 承诺 vs 实际代码）

| Doc 中提到的特性 | 代码状态 |
|---|---|
| AOI (Area of Interest) | Doc 有设计文档，代码未实现 |
| KCP 协议支持 | 设计预留，未实现 |
| 数据库读写 (CRUD) | 只有 Accounts 表的 SELECT |
| 战斗系统 | Doc 有设计，未实现 |
| Buff/技能系统 | Doc 有设计，未实现 |
| 社交系统 (好友/公会/邮件) | SocialServer 为空壳 |
| GM 命令 | Doc 有设计，未实现 |
| 热更新 | Doc 有设计，未实现 |
| 导航/寻路 (NavMesh) | SceneConfig 有 navmeshPath 字段但未使用 |
| 持久化 (存档) | Doc 有设计，未实现 |
| 脚本系统 | Doc 提到 DasLang，未集成 |
| TLS 加密 | Doc 说"后补" |
| 单元测试 | 完全没有 |

---

## 六、优先级排序的行动项

### P0 (立即)
1. 轮换 world.toml 中泄露的数据库密码
2. 修复 CenterServer::_running 原子性

### P1 (本周)
3. 核实并修复 SessionToken 字节序问题
4. 审查 GateServer 锁顺序，统一或合并
5. 添加最小测试框架 + ByteBuffer/Token/Queue/TimingWheel 测试

### P2 (本月)
6. HMAC 截断从 4 字节增大到 8 字节
7. 添加 README.md
8. 修复 catch(...) 诊断信息
9. 确认 LogicThread sessions 独占性有锁或文档化
10. ByteBuffer 字节序使用硬件指令

### P3 (后续)
11. SendToClient 模板代码膨胀优化
12. 配置 schema 验证
13. 补全 SocialServer/AOI/战斗等子系统
