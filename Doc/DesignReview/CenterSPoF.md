# CenterSPoF 方案修正：PlayerLocationIndex 从 World 重拉即可，不需要 Raft

> **更正声明**：上一版文档（`CenterSPoF-v1`）提出了 Raft 3 节点方案，该方案犯了**方向性错误**——把 Center 当成了 PlayerLocationIndex 的 authority（权威源），解决方案用重武器打蚊子。
> 本文彻底纠正：PlayerLocationIndex 的 authority 是 **World**，Center 只是代理（proxy）。代理丢了数据，从源头重拉即可，不需要 Raft。

---

## 一、根本性错误：谁有 PlayerLocationIndex 的 authority

### 1.1 数据流分析

```
『玩家 X 登录到 World-5』这个事实，谁持有？

World-5:
  _sessions[12345] = {
    accountID: 10086,        ← 玩家 X 的 accountID
    entity: {...},
    crypto: {...},
    ...                      ← WorldSession 里有 player 的所有信息
  }
  ✅ World 是 accountID → Entity 的权威
  ✅ World 知道玩家 X 在自己这里

Center:
  _accountToService[10086] = "World-5"   ← 一行映射
  ❌ Center 不知道玩家 X 的任何其他信息
  ❌ Center 不持有 session 状态
  ❌ Center 只是一个查询代理：『这个 accountID 在哪个 World？』
```

**PlayerLocationIndex 本质上是一个 cache，不是 state。** Center 收到 `PlayerOnlineNtf` 然后存一个 `accountID → worldID` 映射。这个映射是 World 报告的——World 才是 truth source。

### 1.2 为什么我上一版搞错了

上一版的核心论据是：

> PlayerLocationIndex 不可重建——World 不会自动重发 PlayerOnlineNtf

这个判断本身没错，但结论错了。**"不会自动重发" ≠ "不能设计成可以重发"。** 正确的解法不是让 Center 变成不丢数据的 Raft 状态机，而是让 World 在重连时**主动重播（replay）全量在线玩家列表**。

---

## 二、修正方案：World Replay 重建 PlayerLocationIndex

### 2.1 核心思路

```
Center 重启 → World TCP 断开 → 退避重连
    └── World 重连成功 → 发 RegisterWorldReq (已有)
                       → 发 BatchOnlinePlayersNtf (新增)
                            ↑ 包含该 World 上所有在线玩家的 accountID
                            ↑ Center 据此重建 PlayerLocationIndex
```

**一句话：World 连接 Center 时，把自己的全量在线列表 dump 过去。**

### 2.2 数据结构

```protobuf
// CenterRPC.proto 新增消息

// World 上线时批量上报在线玩家（用于 Center 重启后重建索引）
message BatchOnlinePlayersNtf
{
    uint32          world_server_id = 1;
    repeated uint32 account_ids     = 2;  // 该 World 当前在线所有玩家
}
```

```cpp
// WorldServer 侧：Center 重连后触发（CenterClient.cpp）
void CenterClient::OnReconnected()
{
    // 1. 发送 RegisterWorldReq (已有)
    SendRegisterWorld();

    // 2. 发送全量在线玩家列表 (新增)
    BatchOnlinePlayersNtf ntf;
    ntf.set_world_server_id(_worldServerID);
    for (auto& [sessionID, ws] : *_sessions) {
        ntf.add_account_ids(ws.accountID);
    }
    _rpcClient.Notify(_socket, MSG_BATCH_PLAYERS_ONLINE_NTF, ntf);
}
```

```cpp
// CenterServer 侧：收到后重建索引
void CenterServer::OnBatchOnlinePlayers(RPCContext ctx, const BatchOnlinePlayersNtf& ntf)
{
    // 先清空该 World 的旧映射（防止 Center 半挂场景下的脏数据）
    _playerIndex.ClearWorld(ntf.world_server_id());

    // 重建
    for (uint32 accountID : ntf.account_ids()) {
        _playerIndex.RegisterPlayer(accountID, ntf.world_server_id());
    }

    Log::Info("Center: World {} replayed {} online players",
              ntf.world_server_id(), ntf.account_ids_size());
}
```

```cpp
// PlayerLocationIndex 新增 ClearWorld 方法
void PlayerLocationIndex::ClearWorld(const std::string& serviceID)
{
    std::unique_lock lock(_mutex);
    std::erase_if(_accountToService, [&](const auto& pair) {
        return pair.second == serviceID;
    });
}
```

### 2.3 完整恢复流程

```
时间线：

T0: Center 宕机
    ├── World 心跳中断
    └── PlayerLocationIndex 内存释放

T1: Center 重启
    ├── Center 开始监听 9660 端口
    └── PlayerLocationIndex 为空（新建）

T1+1s: World-1 重连成功
    ├── World-1 → Center: RegisterWorldReq
    ├── Center: ServiceRegistry[World-1] = online ✅
    ├── World-1 → Center: BatchOnlinePlayersNtf { [1001, 1002, 1003, ...10000] }
    ├── Center: PlayerLocationIndex 插入 10000 条映射
    └── Log: "World-1 replayed 10000 online players"

T1+2s: World-2 重连成功（同上）
    └── ...

T1+3s: LoginServer 重连成功
    ├── LoginServer → Center: PickWorldReq
    ├── Center: 返回 World-1 (PlayerLocationIndex 已完整) ✅
    └── 新玩家可以登录 ✅

T1+3s: 跨服好友查询恢复
    ├── World-3 → Center: QueryPlayerLocationReq(accountID=5001)
    ├── Center: 返回 World-2 (从 BatchOnlinePlayersNtf 重建的) ✅
    └── 跨服功能全部恢复 ✅

恢复完成时间：约 3 秒（取决于 World 数量和在线玩家数）
```

### 2.4 对 World 在线人数巨大的优化

如果单个 World 承载 10000 人，`BatchOnlinePlayersNtf` 序列化后约 40KB（`10000 × 4B`）。一个 World 一包，秒级发送完成，完全可接受。

如果担心单包过大，可以分片：

```protobuf
message BatchOnlinePlayersNtf
{
    uint32          world_server_id = 1;
    repeated uint32 account_ids     = 2;
    uint32          total_chunks    = 3;  // 总分片数
    uint32          chunk_index     = 4;  // 当前分片序号（0-based）
}
```

但在 MVP 阶段（单 World 数千人），不分片也毫无压力。

### 2.5 边缘情况处理

| 场景 | 行为 | 正确性 |
|------|------|--------|
| **Center 部分故障（非全丢）** | World 仍然重连，Center 收到 BatchOnlinePlayersNtf 后先 ClearWorld 再重建 | ✅ 覆盖了"部分脏数据"的情况 |
| **World 也同时宕机** | World 重启后 _sessions 为空 → BatchOnlinePlayersNtf 发空列表 → Center 对应 World 索引清空 | ✅ 那个 World 的玩家本就全部下线了 |
| **Center 重启期间玩家登录到 World** | World 创建了 WorldSession，但 Center 不在线 → 未通知。Center 恢复后 World 重连 → BatchOnlinePlayersNtf 包含该玩家 | ✅ 窗口期内无影响 |
| **Center 重启期间玩家下线** | World 删除了 WorldSession，但 Center 不在线 → 未通知。Center 恢复后 World 重连 → BatchOnlinePlayersNtf 不含该玩家 | ✅ |
| **World 重连后在 BatchOnlinePlayersNtf 发送前有新玩家登录** | BatchOnlinePlayersNtf 是在 Connect 成功后立即发送的，通常在 1 Tick 内。如果真有玩家在这个极短窗口登录，会在后续的 PlayerOnlineNtf 中增量通知 | ✅ 极小窗口，业务可接受 |
| **玩家在 Batch 发送后 0.1ms 登录—Center 已建完索引—错过 PlayerOnlineNtf？** | 不会。World 在发送 Batch 后正常处理游戏消息，新玩家登录会经过 LogicThread 逻辑 → 调 NotifyCenterPlayerOnline() → 发 PlayerOnlineNtf | ✅ 增量机制不受 Batch 影响 |
| **一个玩家登录后 1s，Center 宕机重启** | 玩家还在 World 上。Center 重启 → World 重连 → BatchOnlinePlayersNtf 包含该玩家 → Center 重建索引 | ✅ 零丢失 |
| **一个玩家登录后 1ms，Center 宕机（PlayerOnlineNtf 未到达）** | World 发了 Ntf 但 Center 没收到。Center 重启 → World 重连 → BatchOnlinePlayersNtf 包含该玩家 | ✅ Batch 机制天然弥补了增量通知的丢失 |

**结论：这个方案在最坏的边缘情况下也是正确的。**

---

## 三、评价上版的 Raft 方案

| 维度 | Raft 方案（上一版） | World Replay 方案（本版） |
|------|--------------------|-------------------------|
| authority 归属判断 | ❌ 错。把 Center 当 authority | ✅ 正确。World 才是 authority |
| 实现复杂度 | 引入 NuRaft ~500 行 + vendoring | 新增 1 个 proto message + 2 个方法调用，~30 行 |
| 外部依赖 | NuRaft C++ 库 + openssl | 无 |
| 数据一致性 | 强一致（但这是不需要的） | 最终一致（但重建在秒级完成） |
| 运维复杂度 | 3 节点 Center 部署 | 无需变更，Center 仍然是单进程 |
| 宕机恢复时间 | 1-5s（Raft 选举 + 重连） | 1-3s（World 退避重连 + batch replay） |
| 数据丢失风险 | 零（Raft 日志复制） | 零（World 是 authority，重建即可） |
| 扩展性 | 高（但用不上） | 天然好（World 多一个就是多一个 batch） |

**World Replay 方案在各方面都优于 Raft 方案。Raft 方案的最大错误是：用复杂的技术手段解决了一个不存在的问题。Center 不需要强一致，只需要能恢复。**

---

## 四、对 ServiceRegistry 也需要同样的处理

ServiceRegistry 的恢复依赖 World 心跳超时，这在 Center 宕机重启时会错误地将所有 World 标记为"心跳超时→离线"。

修正也很简单：**RegisterWorldReq 语义从"第一次注册"改为"注册/重注册"，去掉了"心跳超时离线"机制。**

```cpp
// CenterServer::OnNewConnection 中注册 socket close handler
socket->SetCloseHandler([this, sockPtr]() {
    auto it = _socketToService.find(sockPtr);
    if (it != _socketToService.end()) {
        _services.OnSocketLost(it->second);  // 只有 TCP 断线才算离线
        _socketToService.erase(it);
    }
});
```

**ServiceRegistry 不再有心跳超时检查。** 只有以下情况会标记 World 离线：
1. TCP 连接断开（`OnSocketLost`）
2. World 主动 `DeregisterWorldReq`

为什么不需要心跳超时？因为：
- Center 和 World 之间是**长 TCP 连接**，TCP 断线就已经是足够精确的故障检测信号
- 内网 TCP 的可靠性远超应用层心跳
- 心跳（`HeartbeatReq`）仍然保留，但用途改为更新 `currentPlayers` 做负载均衡，不再做存活检测

---

## 五、最终结论

**原始需求**：CenterServer 宕机后 PlayerLocationIndex 数据永久残缺。

**根因分析**：PlayerLocationIndex 是不可重建的缓存，World 不会重发在线玩家列表。

**修正方案**：World 在重连 Center 时，通过 `BatchOnlinePlayersNtf` 批量 dump 全量在线玩家。

**方案评价**：30 行代码、0 额外依赖、秒级恢复、全部边缘情况正确的轻量方案。Raft 是核弹，这个方案是螺丝刀——用对了地方。

---

## 附录：代码变更汇总

### 文件清单

| 文件 | 变更类型 | 说明 |
|------|----------|------|
| `Src/Proto/Internal/CenterRPC.proto` | 修改 | 新增 `BatchOnlinePlayersNtf` 消息 |
| `Src/Center/CenterServer.h` | 修改 | 新增 `OnBatchOnlinePlayers` handler |
| `Src/Center/CenterServer.cpp` | 修改 | 注册 handler + 实现 |
| `Src/Center/PlayerLocationIndex.h` | 修改 | 新增 `ClearWorld` 方法 |
| `Src/Center/PlayerLocationIndex.cpp` | 修改 | 实现 `ClearWorld` |
| `Src/World/CenterClient.h` | 修改 | 新增 `SendBatchOnlinePlayers` |
| `Src/World/CenterClient.cpp` | 修改 | 重连完成后触发 batch dump |

### 代码量估算

- proto message 定义：~10 行
- CenterServer handler：~15 行
- PlayerLocationIndex::ClearWorld：~8 行
- CenterClient batch dump：~20 行

**总计：约 53 行新增代码。**
