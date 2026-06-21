# 基础设施 #7：LoginServer

> 状态：**设计已确认，待实现**
> 关联：[s7_protocol](../前期设计/architecture/s7_protocol.html)（完整登录时序）、[s7b_session_token](../前期设计/architecture/s7b_session_token.html)（SessionToken 签发/验证）、[s2_network](../前期设计/architecture/s2_network.html)（连接拓扑）、[s1_overview](../前期设计/architecture/s1_overview.html)（进程列表 §1.4）

## 1. 定位

LoginServer 是 C2 拓扑中唯一暴露公网的**短 TCP 连接**认证进程。
Client 透过 TLS（或明文 TCP）连接它完成认证后立即断开——后续全由 Gate→World 承载。

**关键设计**：认证完成即断（不是长连接），LoginServer 不持有任何玩家登录状态。

## 2. 决策汇总

| 维度 | 选择 | 理由 |
|------|------|------|
| 架构模式 | **混合模式**（短连接认证 + SessionToken 桥接长连接）| 文档已选定，和 C2 拓扑咬合 |
| 连接模型 | **异步 IOContextPool**（复用 Network TCPSocket） | 和所有进程用同一套 Network 基础设施 |
| 认证失败 | **B + A 组合**（前 3 次回错误码 + 断开，第 4 次起直接关 TCP） | 防暴力破解 |
| argon2id 执行 | **IO 线程直跑** | 登录并发极低（< 10/s），200-500ms CPU 密集对其它连接无感 |
| TLS 第一期 | **明文 TCP（`tls_enabled = false`），预留 SSL 路径** | 认证逻辑和传输层正交，先打通链路 |
| World 选择 | **硬编码 worldId=1（MVP），CenterServer 做好后切换** | 渐进式 |

## 3. 认证流程

按文档 §7.3 的完整时序（6 步），在 IO 线程中同步执行：

```
IO 线程收包 → TCPSocket 拆包 → MessageDispatcher.Dispatch → LoginAuthReq handler (IO 线程内)

  1. Rate Limit 检查（ip 失败 ≥ 3 次且未冷却 → 直接关闭连接，不发错误）
  2. 查 accounts 表 → 取 password_hash + ban_until
  3. argon2id::VerifyPassword(password, password_hash)
  4. 封禁检查（ban_until > now? → 拒绝）
  5. ECDH: GenerateKeyPair → DeriveSharedSecret(clientDHKey, serverPrivKey) → SessionKey = SHA-256(secret)
  6. SessionTokenBuilder::Issue(LSS, SessionKey, worldId, accountId, expireTime)
  7. LoginAuthRsp{gateIPs, serverDHKey.publicKey, sessionToken} → Serialize → socket.Send()
  8. socket.Close()
```

## 4. 反暴力破解

```cpp
class RateLimiter
{
public:
    // 返回 true 则表示允许；false 表示被限制，应直接断开不发错误
    bool Allow(const std::string& ip);
    // 记录一次失败
    void RecordFailure(const std::string& ip);

private:
    struct Entry { int failCount; Clock::time_point cooldownUntil; };
    std::unordered_map<std::string, Entry> _entries;
    static constexpr int    kMaxFailures  = 3;
    static constexpr auto   kCooldown      = 15min;
};
```

## 5. 核心类

### 5.1 LoginServer

```cpp
class LoginServer
{
public:
    bool Init(const LoginConfig& cfg);
    void Run();
    void Stop();

private:
    void OnAuthRequest(std::shared_ptr<TCPSocket> socket,
                       const LoginAuthReq& req,
                       const std::string& clientIP);

    IOContextPool                                 _ioPool;
    TCPAcceptor                                   _acceptor;
    MessageDispatcher<std::shared_ptr<TCPSocket>> _dispatcher;
    RateLimiter                                   _rateLimiter;
    LoginConfig                                   _config;
};
```

### 5.2 LoginConfig

```cpp
struct LoginConfig
{
    struct Network  { uint16 port; int io_threads; bool tls_enabled; std::string cert_path; std::string key_path; } network;
    struct Database { std::string conn_string; int worker_count; } database;
    struct Security { std::string login_server_secret; } security;
    struct World    { std::vector<std::string> gate_ips; uint16 world_server_id; } world;

    static std::optional<LoginConfig> Load(const std::string& path);
};
```

### 5.3 main.cpp

```cpp
int main(int argc, char* argv[])
{
    auto cfg = LoginConfig::Load("Config/login.toml");
    if (!cfg) return 1;

    Log::Init("login");
    DBWorkerPool::Init(cfg->database.worker_count, cfg->database.conn_string);

    LoginServer server;
    if (!server.Init(*cfg)) { Log::Error("Init failed"); return 1; }

    server.Run();
    server.Stop();
    return 0;
}
```

## 6. 配置文件（login.toml）

```toml
[network]
port = 8001
io_threads = 4
tls_enabled = false
# tls_enabled = true 时需下面的证书路径
# cert_path = "Config/certs/login.crt"
# key_path  = "Config/certs/login.key"

[database]
conn_string = "host=127.0.0.1 port=6432 dbname=massive"
worker_count = 3

[security]
login_server_secret = ""

[world]
gate_ips = ["10.0.0.10:9001", "10.0.0.11:9001"]
world_server_id = 1
```

## 7. 对 DB 的依赖

DB 代码生成器产出 `accounts` 表 schema（`AccountsTable.gen.h`）。
LoginServer 用 `DB::Range<AccountsTable>` 查询。

第一期 DB 代码生成器未完成，可**手写 `AccountsTable`** 临时过渡：

```cpp
struct accounts_row
{
    int32_t     account_id = 0;
    std::string username;
    std::string password_hash;
    std::string email;
    Timestamp   ban_until;
    Timestamp   created_at;
    Timestamp   last_login_at;
};

struct AccountsTable
{
    static constexpr auto kTableName = "accounts";
    using RowType  = accounts_row;
    using PKType   = int32_t;

    static constexpr auto account_id    = Column<int32_t>{"account_id",    kPK | kAutoInc};
    static constexpr auto username      = Column<std::string>{"username",    kRequired};
    static constexpr auto password_hash = Column<std::string>{"password_hash", kRequired};
    static constexpr auto email         = Column<std::string>{"email",       kNone};
    static constexpr auto ban_until     = Column<Timestamp>{"ban_until",     kNone};
    static constexpr auto created_at    = Column<Timestamp>{"created_at",    kRequired};
    static constexpr auto last_login_at = Column<Timestamp>{"last_login_at", kNone};
    static constexpr auto PK = account_id;
};
```

## 8. 文件清单

```
Src/Login/
├── main.cpp                   # 进程入口
├── LoginServer.h / .cpp       # 认证主逻辑
├── LoginConfig.h / .cpp       # 强类型配置
├── RateLimiter.h              # 反暴力破解
Src/Common/DB/
├── AccountsTable.h            # 手写临时 schema（DB 代码生成器完成前）

Config/
└── login.toml                 # Login 专属配置
```

## 9. 依赖状态

| 依赖 | 基础设施 | 第一期可用？ |
|------|----------|-------------|
| IOContextPool | #1 Network | ✅ 已有 |
| TCPSocket + TCPAcceptor | #1 Network | ⚠️ 待实现 |
| toml++ / ConfigLoader | #2 Config | ⚠️ 待实现 |
| argon2id | #5 Crypto | ⚠️ 待实现 |
| DBWorkerPool | 已有 | ✅ |
| AccountsTable | #6 DB 代码生成 | ⚠️ 手写 schema 过渡 |
| SessionToken / ECDH | 已有 CommonCrypto | ✅ |
| Proto / MsgID | 已有 | ✅ |
| MessageDispatcher | 已有 | ✅ |
