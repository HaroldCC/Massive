# 基础设施 #4：CenterServer

> 状态：**设计已确认，待实现**
> 关联：[center_server](../前期设计/architecture/center_server.html)（ServiceRegistry/PlayerLocationIndex/RPC 协议）、[s1_overview](../前期设计/architecture/s1_overview.html)（进程拓扑）

## 1. 定位

CenterServer 是分布式集群的**服务协调中心**——纯 IO 进程，无 LogicThread，无 DB。
WorldServer 启动时注册、定期心跳，Center 维护服务表 + 玩家位置索引，提供 PickLeastLoadedWorld。

## 2. 决策汇总

| 维度 | 选择 | 理由 |
|------|------|------|
| 玩家索引 key | **accountId (uint32)** | 内部路由走数字 ID，hash 快；名字查询留给 SocialServer |
| 服务发现 | **TCP 断线即离线 + 心跳兜底（30s）** | 内网 TCP 稳定，断线即时感知；心跳兜底防边缘情况 |
| 崩溃恢复 | **无状态重启 + World 重注册** | Center 无业务状态，重启后 World 自发重建，简单可靠 |
| 线程模型 | 纯 IO 线程（无 LogicThread） | 服务注册表 + 索引用锁或无锁容器保护 |
| RPC handler 线程 | IO 线程直跑（无 LogicThread 可投递） | 操作 ServiceRegistry/PlayerLocationIndex 用读写锁 |

## 3. 核心类

### 3.1 ServiceRegistry（服务注册表）

```cpp
class ServiceRegistry
{
public:
    struct ServiceInfo
    {
        std::string serviceID;
        std::string address;
        uint32      maxPlayers;
        uint32      currentPlayers;
        bool        online;
    };

    void Register(const ServiceInfo& info);   // World 上线
    void Heartbeat(const std::string& id, uint32 currentPlayers);
    void Deregister(const std::string& id);   // World 主动下线
    void OnSocketLost(const std::string& id); // TCP 断线

    // 最少负载选择（LoginServer 调用）
    const ServiceInfo* PickLeastLoadedWorld() const;

    // 检查超时心跳
    void CheckTimeouts();

private:
    std::shared_mutex _mutex;  // 读多写少，读写锁
    std::unordered_map<std::string, ServiceInfo> _services;
    static constexpr auto kHeartbeatTimeout = 30s;
};
```

### 3.2 PlayerLocationIndex（玩家位置索引）

```cpp
class PlayerLocationIndex
{
public:
    void RegisterPlayer(uint32 accountID, const std::string& serviceID);
    void UnregisterPlayer(uint32 accountID);
    std::optional<std::string> GetServiceID(uint32 accountID) const;
    uint32 GetTotalOnline() const;

private:
    std::shared_mutex _mutex;
    std::unordered_map<uint32, std::string> _accountToService;
};
```

### 3.3 CenterServer（主类）

```cpp
class CenterServer
{
    ServiceRegistry      _services;
    PlayerLocationIndex  _playerIndex;
    // RPC handler 注册
    MessageDispatcher<RPCContext> _rpcHandlers;
    // 内部 TCP 监听（接受 World/Social 连接）
    TCPAcceptor          _acceptor;
};
```

## 4. RPC handler（待实现）

注册在 `Src/Proto/Internal/CenterRPC.proto`，由内部 MsgID 生成器分配 `EInternalMsgID`：

```protobuf
// 服务发现
message RegisterWorldReq { ... }
message RegisterWorldRsp { bool ok = 1; }
message HeartbeatReq { uint32 current_players = 1; }
message HeartbeatRsp { bool ok = 1; }

// 玩家位置
message PlayerOnlineReq { uint32 account_id = 1; }   // Notify
message PlayerOfflineReq { uint32 account_id = 1; }  // Notify
message QueryPlayerLocationReq { uint32 account_id = 1; }
message QueryPlayerLocationRsp { uint32 world_server_id = 1; }

// 最少负载
message PickWorldReq { }
message PickWorldRsp { string world_server_id = 1; string address = 2; }
```

## 5. 文件清单

```
Src/Center/
├── main.cpp
├── CenterServer.h / .cpp       # 主类
├── ServiceRegistry.h / .cpp    # 服务注册表
├── PlayerLocationIndex.h / .cpp # 玩家位置索引
├── CenterConfig.h / .cpp       # Center 专属配置
Src/Proto/Internal/
└── CenterRPC.proto             # 内部 RPC 消息定义
Config/
└── center.toml
```

## 6. 未决/后续

- **主备切换**：Center 单点，加主备后需状态同步——远期。
- **Center 监控指标**：在线 World 数/玩家数 Prometheus——运营期。
- **全服公告/广播**：GM 工具集成——远期。
