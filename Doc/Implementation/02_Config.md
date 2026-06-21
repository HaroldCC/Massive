# 基础设施 #2：配置系统（Config）

> 状态：**设计已确认，待实现**
> 关联：[center_server](../前期设计/architecture/center_server.html)（[center] address）、[s7b_session_token](../前期设计/architecture/s7b_session_token.html)（[security] login_server_secret）

## 1. 目标

每个进程启动时读取配置：监听端口、IO 线程数、Center 地址、DB 连接串、
LSS 密钥、World 列表等。提供类型安全、配置集中、业务代码不耦合解析库的配置访问。

## 2. 决策汇总

| 维度 | 选择 | 理由 |
|------|------|------|
| 格式 | **TOML** | 人类友好、强类型、支持注释、分 section 清晰；文档已采用 server.toml |
| 解析库 | **toml++**（vendored header-only）| C++ TOML 事实标准，单头文件，契合现有 vendoring 策略 |
| 访问方式 | **强类型 Config 结构体**（手写映射）| 类型安全、配置集中、业务代码不依赖 toml++、符合"零宏"倾向 |
| 多进程组织 | **每进程独立 toml**（起步）| 简单；公共配置真的变多再抽 common.toml |

## 3. 依赖：toml++

新增 vendored 依赖 `ThirdParty/tomlplusplus/`（header-only）。

```lua
-- ThirdParty/xmake.lua 新增
target("TomlPlusPlus")
    set_kind("headeronly")
    add_includedirs("$(projectdir)/ThirdParty/tomlplusplus/include", {public = true})
```

## 4. CommonConfig：加载工具

`Src/Common/Config/` 提供"加载 toml + 安全取值"的基础工具，
各进程在此之上定义自己的强类型 Config 结构体。

```cpp
// Src/Common/Config/Config.h
namespace MMO
{

/**
 * @brief TOML 配置加载工具
 *
 * 封装 toml++，提供安全取值（带默认值、缺失不崩溃）。
 * 各进程定义自己的 Config 结构体，在 Load 中调用本工具填充。
 */
class ConfigLoader
{
public:
    // 加载 toml 文件，失败返回 false（文件不存在/语法错误）
    bool LoadFile(const std::string& path);

    // 取值（path 用点分：如 "network.port"），缺失返回 default
    int         GetInt(const std::string& path, int defaultVal) const;
    uint16      GetUInt16(const std::string& path, uint16 defaultVal) const;
    std::string GetString(const std::string& path, const std::string& defaultVal) const;
    bool        GetBool(const std::string& path, bool defaultVal) const;

    // 取字符串数组（如 World 列表）
    std::vector<std::string> GetStringArray(const std::string& path) const;

private:
    // toml::table（pImpl 隐藏 toml++ 依赖，业务侧不 include toml++）
    void* _table = nullptr;
};

}
```

> 设计要点：`ConfigLoader` 用 pImpl 隐藏 toml++，使业务代码只依赖 `Config.h`，不直接 include toml++ 头文件，降低编译耦合。

## 5. 进程专属 Config 结构体（示例）

每个进程在自己目录定义强类型配置结构：

```cpp
// Src/Login/LoginConfig.h
namespace MMO
{

struct LoginConfig
{
    struct Network
    {
        uint16 port = 8001;
        int    ioThreads = 4;
    } network;

    struct Database
    {
        std::string connString = "host=127.0.0.1 port=6432 dbname=massive";
        int         workerCount = 3;
    } database;

    struct Security
    {
        std::string loginServerSecret;  // LSS（32B，hex 或 base64）
    } security;

    struct WorldList
    {
        std::vector<std::string> gateIPs;  // 返回给客户端的 Gate 地址
    } worldList;

    // 从 toml 文件加载，失败返回 nullopt
    static std::optional<LoginConfig> Load(const std::string& path);
};

}
```

```cpp
// Src/Login/LoginConfig.cpp
std::optional<LoginConfig> LoginConfig::Load(const std::string& path)
{
    ConfigLoader loader;
    if (!loader.LoadFile(path))
        return std::nullopt;

    LoginConfig cfg;
    cfg.network.port        = loader.GetUInt16("network.port", 8001);
    cfg.network.ioThreads   = loader.GetInt("network.io_threads", 4);
    cfg.database.connString = loader.GetString("database.conn_string", cfg.database.connString);
    cfg.database.workerCount= loader.GetInt("database.worker_count", 3);
    cfg.security.loginServerSecret = loader.GetString("security.login_server_secret", "");
    cfg.worldList.gateIPs   = loader.GetStringArray("world_list.gate_ips");
    return cfg;
}
```

## 6. 配置文件示例

```toml
# Config/login.toml

[network]
port = 8001
io_threads = 4

[database]
conn_string = "host=127.0.0.1 port=6432 dbname=massive"
worker_count = 3

[security]
# LSS：与 WorldServer 共享，用于 SessionToken 加密/签名
# 生产环境应通过环境变量 MASSIVE_LSS 注入，不写在文件里
login_server_secret = ""

[world_list]
gate_ips = ["10.0.0.10:9001", "10.0.0.11:9001"]
```

## 7. 命名约定

| 元素 | 命名 |
|------|------|
| 加载工具 | `ConfigLoader` |
| 进程配置结构 | `LoginConfig` / `GateConfig` / `WorldConfig` ... |
| toml section | snake_case（`world_list`、`io_threads`）|
| C++ 字段 | 小驼峰（`ioThreads`、`gateIPs`）|

toml 内部用 snake_case（配置文件惯例），映射到 C++ 小驼峰字段。

## 8. 文件清单（待实现）

```
ThirdParty/tomlplusplus/         # 新增 vendored（header-only）
Src/Common/Config/
├── xmake.lua                    # CommonConfig target
├── Config.h / .cpp              # ConfigLoader（pImpl 封装 toml++）
各进程目录/
└── XxxConfig.h / .cpp           # 进程专属强类型配置
Config/
├── login.toml
├── gate.toml
└── ...
```

## 9. 未决/后续

- **环境变量覆盖**：敏感配置（LSS、DB 密码）支持 `MASSIVE_LSS` 等环境变量覆盖文件值
  （s7b_session_token §3 已提及）。本期先支持文件，环境变量后补。
- **公共配置抽取**：进程数增多、公共配置（日志、DB）重复变多时，
  抽 `Config/common.toml` 分层加载。
- **配置热重载**：运营配置（非启动参数）的热重载，远期。
