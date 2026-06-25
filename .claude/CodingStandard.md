# MMO Server 编码规范 v2.0

> 基于设计文档 §10.4，结合项目 C++23 + EnTT + DasLang + Protobuf 技术栈制定。
> 本规范优先级：本文 > `.clang-tidy` > `.clang-format`，如有冲突以本文为准。

---

## 1. 命名规范

### 1.1 核心总表

| 元素 | 规范 | 示例 | 说明 |
|------|------|------|------|
| 类名/结构体名 | 大驼峰 (PascalCase) | `LogicServer`, `Health` | 含首字母缩写全大写：`IOContextPool`, `DBWorkerPool` |
| 抽象类/接口 | 大驼峰 + `I` 前缀 | `IScene`, `IModule` | 含纯虚函数的类型 |
| 命名空间 | 大写缩写 | `MMO`, `MMO::Network`, `MMO::DB` | 顶层 `MMO`，二级全大写缩写 |
| 函数名 | 大驼峰 | `GetNextContext()`, `HandleMove()` | 含成员函数和自由函数 |
| 类成员变量 | 小驼峰 + `_` 前缀 | `_ioContexts`, `_sceneID` | 含 private/protected 非静态成员 |
| 类静态变量 | 小驼峰 + `_` 前缀 | `_instance`, `_maxPoolSize` | 含 private/protected 静态成员 |
| 类静态常量 | 大驼峰 + `k` 前缀 | `kHotThreshold`, `kMaxPacketSize` | constexpr / static const |
| 结构体成员 | 小驼峰（无前缀） | `current`, `sessionID` | 公开的数据聚合 |
| 局部变量 | 小驼峰 | `entityID`, `totalLength` | — |
| 函数参数 | 小驼峰 | `sessionID`, `msgID` | — |
| 全局变量 | 小驼峰 + `g_` 前缀 | `g_ConfigPath`, `g_IsRunning` | 仅限极少数全局单例入口 |
| 枚举类型名 | 大驼峰 + `E` 前缀 | `EMsgID`, `ESceneType` | — |
| 枚举值 | 全大写 + 下划线 | `MSG_MOVE_REQ`, `SCENE_MAIN_CITY` | 不带枚举类型名前缀 |
| 宏 | 全大写 + 下划线 | `REGISTER_COMPONENT`, `MASSIVE_ASSERT` | 项目前缀 `MASSIVE_` |
| 模板参数 | 大驼峰 | `typename T`, `typename TableSchema` | 单字母大写或描述性大驼峰 |
| 类型别名 | 大驼峰 | `using WorkPtr = ...`, `using SessionMap = ...` | — |
| 概念 (concept) | 大驼峰 | `template<typename T> concept IsComponent` | — |

### 1.2 ID 后缀规则

所有标识符中的 `Id` 后缀统一写作 `ID`（大写）。适配所有常见组合：

| 旧写法 | 新写法 | 出现场景 |
|--------|--------|----------|
| `sessionId` / `session_id` | `sessionID` / `session_id` | Session 标识 |
| `sceneId` / `scene_id` | `sceneID` / `scene_id` | Scene 标识 |
| `entityId` / `entity_id` | `entityID` / `entity_id` | Entity 标识 |
| `msgId` / `msg_id` | `msgID` / `msg_id` | 消息 ID |
| `traceId` / `trace_id` | `traceID` / `trace_id` | 分布式追踪 |
| `playerId` / `player_id` | `playerID` / `player_id` | 玩家 |
| `accountId` / `account_id` | `accountID` / `account_id` | 账号 |
| `guildId` / `guild_id` | `guildID` / `guild_id` | 公会 |
| `templateId` / `template_id` | `templateID` / `template_id` | 模板 |
| `classId` / `class_id` | `classID` / `class_id` | 职业 |
| `skillId` / `skill_id` | `skillID` / `skill_id` | 技能 |
| `questId` / `quest_id` | `questID` / `quest_id` | 任务 |
| `itemId` / `item_id` | `itemID` / `item_id` | 物品 |
| `dungeonId` / `dungeon_id` | `dungeonID` / `dungeon_id` | 副本 |
| `instanceId` / `instance_id` | `instanceID` / `instance_id` | 实例 |

> **注意**：Protobuf 字段名使用 `snake_case`，其中的 `_id` 保持小写（如 `session_id`），
> C++ 生成代码自动转为 `sessionID`（PascalCase accessor）。

### 1.3 类名中的首字母缩写

类名/类型名中的常见缩写（2-4 个字母）保持全大写：

| 缩写 | 示例 | 说明 |
|------|------|------|
| IO | `IOContextPool` | I/O 上下文池 |
| DB | `DBWorkerPool`, `DBRange`, `DBResult` | 数据库相关 |
| ECS | `ECSSystem`, `RegisterECSComponent()` | ECS 框架 |
| AOI | `AOIManager`, `AOIGrid` | 兴趣区域 |
| ID | `EMsgID`, 各 ID 后缀 | 标识符 |
| RPC | `RPCHandler`, `RPCClient` | 远程调用 |
| GM | `GMCommand`, `GMPermission` | 游戏管理 |
| AI | `AIBlackboard`, `AIBehaviorTree` | 人工智能 |
| HP | `HPCurrent` → 推荐 `CurrentHP` | 生命值（放后面更自然） |
| MP | `MPCurrent` → 推荐 `CurrentMP` | 魔法值 |
| NPC | `NPCTemplate`, `NPCSpawner` | 非玩家角色 |

### 1.4 协议消息命名

| 方向 | 后缀 | 示例 |
|------|------|------|
| 客户端 → 服务器 | `_Req` | `MSG_MOVE_REQ` |
| 服务器 → 客户端 | `_Rsp` | `MSG_MOVE_RSP` |
| 服务器主动推送 | `_Ntf` | `MSG_ENTITY_CREATE_NTF` |

### 1.5 文件命名（全部大驼峰）

| 文件类型 | 命名 | 示例 |
|----------|------|------|
| C++ 头文件 | 大驼峰 `.h` | `LogicServer.h`, `IOContextPool.h` |
| C++ 源文件 | 大驼峰 `.cpp` | `LogicServer.cpp`, `IOContextPool.cpp` |
| 自动生成头文件 | 大驼峰 `.gen.h` | `PlayersTable.gen.h`, `GuildsTable.gen.h` |
| 自动生成源文件 | 大驼峰 `.gen.cpp` | `AllTables.gen.cpp` |
| DasLang 脚本 | 大驼峰 `.das` | `ServerTick.das`, `CombatSystem.das` |
| Protobuf 文件 | 大驼峰 `.proto` | `MsgMove.proto`, `Common.proto`, `MsgID.proto` |
| 配置文件 | 大驼峰 `.toml` | `Server.toml`, `WorldConfig.toml` |
| CMake/Xmake | `xmake.lua`, `CMakeLists.txt` | — |

> **统一原则**：除构建系统固定文件名外，所有文件一律大驼峰。

### 1.6 目录结构（全部大驼峰）

```
Massive/
├── xmake.lua                    # 顶层构建入口
│
├── Src/
│   ├── Common/                  # 共享库（所有进程共用）
│   │   ├── xmake.lua            #   聚合入口——不构建实体，仅 includes 子 target
│   │   ├── Core/                #   target: CommonCore — 基础类型、断言、公共工具
│   │   ├── DB/                  #   target: CommonDB — DBWorkerPool, DBRange<T>, Column
│   │   │   └── AutoGen/         #     自动生成的 TableSchema (*.gen.h)
│   │   ├── Network/             #   target: CommonNetwork — IOContextPool, PacketHeader
│   │   ├── Queue/               #   target: CommonQueue — MPSCQueue（moodycamel 封装）
│   │   ├── Crypto/              #   target: CommonCrypto — Aes256Gcm, ECDH, SessionToken
│   │   ├── ECS/                 #   target: CommonECS — Entity, Scene, ComponentMap, ScriptComponentStorage
│   │   ├── Timer/               #   target: CommonTimer — TimingWheel
│   │   └── Log/                 #   target: CommonLog — LogService, spdlog 封装
│   │
│   ├── Login/                   # target: LoginServer
│   ├── Gate/                    # target: GateServer
│   ├── World/                   # target: WorldServer
│   │   ├── Scene/               #   Scene 生命周期, AOI, NavMesh
│   │   ├── Handler/             #   消息处理函数
│   │   ├── System/              #   C++ ECS 系统（Movement, Combat, AI...）
│   │   └── RPC/                 #   Center/Social RPC 客户端
│   ├── Center/                  # target: CenterServer
│   ├── Social/                  # target: SocialServer
│   └── Proto/                   # target: Proto — Protobuf 编译
│
├── ThirdParty/                  # 纯源码 vendoring（不依赖 xrepo）
│   ├── asio/                    #   1.36.0  Boost.Asio standalone（header-only）
│   ├── concurrentqueue/         #   1.0.5   moodycamel::ConcurrentQueue（header-only）
│   ├── daScript/                #   master  DasLang 运行时（source）
│   ├── entt/                    #   3.16.0  ECS 框架（header-only）
│   ├── fmt/                     #   12.2.0  {fmt} 格式化库（header-only）
│   ├── Libpq/                   #   18.4    PostgreSQL C 客户端（需预编译）
│   ├── openssl/                 #   4.0.1   libssl + libcrypto（需预编译）
│   ├── protobuf/                #   35.1    序列化（需 cmake 构建）
│   ├── spdlog/                  #   1.17.0  日志库（header-only）
│   └── tracy/                   #   0.13.1  性能分析器（source）
│
├── Script/                      # DasLang 脚本
│   ├── System/                  #   游戏系统脚本
│   ├── Component/               #   脚本组件定义
│   └── Lib/                     #   脚本库函数
│
├── Config/                      # 配置文件模板
├── Tools/                       # 代码生成、部署工具
│
├── Test/
│   ├── Unit/                    #   单元测试
│   ├── Integration/             #   集成测试
│   └── Benchmark/               #   性能压测（BenchmarkScene）
│
├── Doc/                         # 设计文档
│
├── .clang-format
├── .clang-tidy
├── .claude/
│   ├── CLAUDE.md
│   └── CodingStandard.md
└── .gitignore
```

> **xmake target 依赖拓扑**：
> ```
> CommonCore ───────────────────────────── 无依赖，最底层
> CommonDB ──────→ CommonCore
> CommonNetwork ─→ CommonCore
> CommonQueue ─── 无依赖
> CommonCrypto ── 无依赖
> CommonECS ─────→ CommonCore
> CommonTimer ─── 无依赖
> CommonLog ─────→ CommonCore
> Proto ───────── 无依赖
> 
> LoginServer ───→ CommonCore, CommonDB, CommonCrypto, Proto
> GateServer ────→ CommonCore, CommonNetwork, CommonQueue, CommonCrypto, Proto
> WorldServer ───→ CommonCore, CommonDB, CommonNetwork, CommonQueue,
>                   CommonCrypto, CommonECS, CommonTimer, CommonLog, Proto
> CenterServer ──→ CommonCore, CommonNetwork, Proto
> SocialServer ──→ CommonCore, CommonDB, CommonNetwork, CommonLog, Proto
> ```

---

## 2. 格式化规范

### 2.1 大括号风格 — Allman

**统一使用 Allman 风格**：开括号换行独占一行。

```cpp
// ✅ 正确 — Allman 风格
class LogicServer
{
public:
    void Run()
    {
        while (!_stopped.load())
        {
            ProcessMessages();
        }
    }
};

if (elapsed > maxElapsed)
{
    elapsed = maxElapsed;
}

// ❌ 错误 — K&R 风格
class LogicServer {
public:
    void Run() {
        // ...
    }
};
```

### 2.2 缩进与空格

| 项目 | 设置 |
|------|------|
| 缩进宽度 | 4 空格 |
| Tab 字符 | 禁止（UseTab: Never） |
| 每行最大字符数 | 110 |
| 指针/引用对齐 | 靠右 `int* p`, `const std::string& s` |
| 限定符顺序 | `inline static const type` |
| 访问修饰符偏移 | -4（与 class 关键字对齐） |

### 2.3 空格规则

```cpp
// 控制语句关键字后括号前加空格
if (condition)
{
}
for (auto& x : items)
{
}
while (running)
{
}

// 函数调用/定义括号前不加空格
DoSomething(a, b);
void Foo(int x);

// 模板关键字后加空格
template <typename T>
template <>

// 花括号列表前加空格
std::vector<int> v = {1, 2, 3};

// 尾随注释前加 1 个空格
int count = 0;  // 当前计数

// 范围 for 冒号前加空格
for (auto& entity : entities)
{
}

// 赋值运算符前后加空格
int x = 5;

// 尖括号内不加空格
std::vector<int>   // ✅
std::vector< int > // ❌
```

### 2.4 换行规则

- 参数和实参不打包（`BinPackArguments: false`, `BinPackParameters: false`）：要么全同行，要么全分行
- 构造函数初始化列表在逗号前换行
- 继承列表在逗号前换行
- 二元运算符在操作符前换行（非赋值类）
- 三元运算符在操作符前换行
- 函数返回类型后不强制换行
- `public:` / `private:` 等标签不缩进

```cpp
// 长参数列表——每行一个
void ProcessEntity(
    uint32 entityID,
    const Position& pos,
    const Velocity& vel,
    float deltaTime);

// 构造函数初始化列表——逗号前换行，每行一个
Scene(uint32 sceneID)
    : _sceneID(sceneID)
    , _ecsRegistry()
    , _ready(false)
{
}

// 继承列表——逗号前换行
class GateSession
    : public std::enable_shared_from_this<GateSession>
    , public IConnection
{
```

### 2.5 空行规则

- 连续空行最多 1 行
- 定义块之间始终用空行分隔（`SeparateDefinitionBlocks: Always`）
- 块起始处不保留空行

### 2.6 其他格式

- 自动在命名空间末尾添加 `// namespace MMO` 注释
- 自动排序 using 声明
- 自动插入 if/else/while/for 的花括号（`InsertBraces: true`）
- 不自动排序 `#include`（手动控制顺序）
- 要求 require 子句独占一行

---

## 3. 注释规范

### 3.1 总则

- **全部使用 Doxygen `/** */` 风格**，配合 `@brief`、`@param`、`@return`、`@note`、`@tparam`、`@warning` 等标签
- xmake.lua 使用 `---` 替代（lua 注释风格）
- **不参与 Doxygen** 的内容继续用 `//`：行内短注、TODO/FIXME/HACK
- 禁止出现**裸 `//` 注释替代 Doxygen**的场景：函数实现前、变量声明前、枚举值后、文件头等应参与文档生成的必须使用 `/** */`

### 3.2 分组注释（@name）

功能模块分组使用 `/** @name 分组名 */` + `/** @{ */` `/** @} */` 形式。当分组名足以自解释时，可省略 `@name` 直接用 `/** @defgroup ... */`：

```cpp
    /** @name 生命周期 */

    /**
     * @brief 初始化 spdlog
     * @param name  日志器名称
     */
    static void Init(const std::string& name);
    
    /** @brief 关闭 spdlog */
    static void Shutdown();
    
    /** @name traceID 上下文 */

    /** @brief 设置当前线程的 traceID */
    static void SetTraceID(uint64 traceID);
    /** @brief 获取当前线程的 traceID */
    static uint64 GetTraceID();
```

### 3.3 文件头注释

```cpp
/**
 * @file LogicServer.h
 * @brief WorldServer 单线程逻辑循环核心
 *
 * 负责消息分发、Tick 驱动、过载保护
 */
#pragma once
```

```cpp
/**
 * @file PlayersTable.gen.h
 * @brief 自动生成文件
 *
 * 来源: PostgreSQL table 'players'
 * 生成工具: Tools/generate_db_bindings.py
 * @warning 不要手动编辑
 */
#pragma once
```

### 3.4 类/函数注释

```cpp
/**
 * @brief N 个独立 asio::io_context 的线程池
 *
 * 每个 io_context 运行于独立线程，Round-Robin 分配连接，
 * 同一连接始终在同一线程，零锁竞争
 */
class IOContextPool
{
public:
    /**
     * @brief 获取下一个 io_context（Round-Robin）
     * @return asio::io_context&，线程安全
     */
    asio::io_context& GetNextContext();

    /** @brief 停止所有 io_context 并 join 所有工作线程 */
    void Stop();
};
```

#### 完整标签示例（公开方法）

```cpp
/**
 * @brief 对 plaintext 进行 AES-256-GCM 加密
 * @param key  32B AES-256 密钥
 * @param iv   12B nonce
 * @param plaintext    明文数据指针
 * @param plaintextLen 明文长度
 * @return ciphertext + 16B GCM tag 的 ByteBuffer，失败返回 nullopt
 */
static std::optional<ByteBuffer> Encrypt(
    const uint8* key,
    const uint8* iv,
    const uint8* plaintext,
    size_t plaintextLen);
```

#### 模板函数——@tparam 标签

模板函数必须使用 `@tparam` 标注模板参数含义：

```cpp
/**
 * @brief 分发日志，无参路径跳过格式化（零开销）
 * @tparam Args  格式化参数类型
 */
template <typename... Args>
void LogDispatch(ELogLevel level, const FormatWithLocation& f, Args&&... args);
```

#### 补充说明——@note 标签

需要强调但不属于 `@param`/`@return` 的约束或注意事项使用 `@note`：

```cpp
/**
 * @brief 隐式捕获 caller 的 source_location + 编译期缩短 file/func
 *
 * @note 缩短后的 std::string_view 指向编译器静态存储的原始字符串字面量，运行时安全。
 */
struct FormatWithLocation
{
    // ...
};
```

#### 枚举注释（行尾 `/**<`）

```cpp
/**
 * @brief 全项目统一错误码
 *
 * 与 C++23 std::expected 搭配：
 *   std::expected<Entity, EErrorCode> CreatePlayer(const std::string& name);
 */
enum class EErrorCode : uint32
{
    OK                 = 0,  /**< 成功 */
    UNKNOWN            = 1,  /**< 未分类错误 */
    INVALID_ARGUMENT   = 2,  /**< 非法参数 */
    OUT_OF_RANGE       = 3,  /**< 越界 */
};
```

### 3.5 实现文件（.cpp）注释

```cpp
/**
 * @file TimingWheel.cpp
 * @brief 三级时间轮定时器实现
 */

/** @brief 全局 TimerID 生成器（永不回绕） */
std::atomic<TimingWheel::TimerID> TimingWheel::_nextTimerID{1};

/** @brief 生成唯一 TimerID */
TimingWheel::TimerID TimingWheel::GenTimerID()
{
    return _nextTimerID.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief 从对象池分配节点
 * @param id      TimerID
 * @param rounds  剩余轮数
 * @param cb      到期回调
 * @return 节点指针（池中有空闲则复用，否则 new）
 */
TimingWheel::Node* TimingWheel::AllocNode(TimerID id, int rounds, Callback cb)
{
    // ...
}
```

| 注解位置 | 规则 |
|----------|------|
| 顶格不缩进的 `/** @brief` | 文件作用域实现（静态变量、函数实现） |
| 缩进的 `/** @brief` | 方法实现 |
| 匿名命名空间中的类 | 类前加 `/** @brief */`，内部函数用 `//` |
| `// =====` 分隔线 | **不允许**——改用 `/** @brief */` 直接标注 |

### 3.6 xmake.lua 注释

```lua
--- @file xmake.lua
--- @brief ThirdParty — 第三方依赖统一声明
---
--- A 组: 外部编译产物 (OpenSSL, Protobuf, daScript, libpq)
--- B 组: xmake 内编译为静态库 (Fmt, TracyClient)
--- C 组: 纯头文件 (Asio, EnTT, Spdlog, ConcurrentQueue)

--- @section A 组: 外部编译产物

target("OpenSSL")
    set_kind("headeronly")
    if is_plat("windows") then
        --- Windows: vendored 二进制
        add_includedirs("$(projectdir)/ThirdParty/Bin/openssl/include", {public = true})
```

### 3.7 行内注释

```cpp
// 解释"为什么"，而不是"是什么"
bool _routed = false;  // Gate 侧：首次路由完成后置 true

// 关键参数注释
HandleMove(entity, x, y);  // x/y 已通过反外挂校验

// 阶段标记（不参与 Doxygen）
// ── Phase 1: Drain Messages ──
```

### 3.8 TODO / FIXME / HACK

```cpp
// TODO(cheng): 多线程升级——每个 Scene 一个 LogicThread
// FIXME: DrainAll 需加容量上限，否则内存可能被打爆
// HACK: 临时绕过 EnTT 4.0 的 group size 在脚本层获取问题
```

---

## 4. C++ 代码规范

### 4.1 头文件

```cpp
#pragma once  // ✅ 使用 #pragma once

#include <cstdint>        // 标准库
#include <string>
#include <vector>

#include <asio.hpp>       // 第三方库

#include "Common/Entity.h"  // 项目头文件
#include "Common/LogService.h"
```

> **include 顺序**：标准库 → 第三方库 → 项目头文件。每组内按字母序。

### 4.2 类型使用

```cpp
// 定宽整数——使用 Common/Core/Types.h 中定义的别名（基于 <cstdint>）
#include "Common/Core/Types.h"

uint32 sceneID;   // ✅
int32  damage;    // ✅
uint64 traceID;   // ✅
// 禁止: int, long, unsigned int（在不同平台宽度不一致）

// 可用别名：
// int8 / int16 / int32 / int64
// uint8 / uint16 / uint32 / uint64

// 浮点——游戏逻辑用 float（精度够用、省内存），数值计算用 double
float posX, posY;   // ✅ 游戏坐标
double elapsed;     // ✅ 时间差计算
// 禁止: float32 / float64 等别名——保留 float / double 原始写法
```

### 4.3 表达式与语句

```cpp
// auto 使用原则：类型明显时使用 auto，否则显式写类型
auto* node = new Node{};                                  // ✅ 类型明显
auto it = _sessionToEntity.find(sessionID);               // ✅ 避免冗长迭代器类型
auto e = _ecsRegistry.create();                           // ✅ EnTT entity
std::vector<LogicMessage> messages;                       // ✅ 类型不够明显，显式写

// nullptr 而非 NULL 或 0
Node* next = nullptr;  // ✅
// Node* next = NULL;  // ❌

// 用 using 而非 typedef
using SessionMap = std::unordered_map<uint32, Entity>;   // ✅
// typedef std::unordered_map<uint32, Entity> SessionMap; // ❌

// constexpr 优先于 const（编译期不变量）
static constexpr size_t kMaxHandlers = 4096;
static constexpr auto kTickInterval = std::chrono::milliseconds(50);
```

### 4.4 类设计规范

```cpp
// 成员顺序：public → protected → private
class Entity
{
public:
    // 1. 类型定义
    using IDType = uint64;

    // 2. 静态常量
    static constexpr uint32 kSceneGlobal = 0;
    static constexpr uint32 kScenePersistent = 0xFFFFFFFF;

    // 3. 构造/析构
    Entity() = default;

    // 4. 公开方法
    uint32 SceneID() const
    {
        return uint32(id >> 32);
    }
    uint32 EntityID() const
    {
        return uint32(id & 0xFFFFFFFF);
    }

private:
    // 5. 成员变量
    uint64 id;
};
```

### 4.5 结构体设计规范

结构体用于数据聚合（如 Component），成员公开访问，无 getter/setter。

```cpp
// Component — 纯数据，无行为
struct Position
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// 消息结构
struct LogicMessage
{
    uint32 sessionID;
    uint32 msgID;
    uint64 traceID;
    uint64 clientTraceID;
    std::vector<char> body;
    std::chrono::steady_clock::time_point recvTime;
};
```

### 4.6 接口规范

```cpp
// 抽象接口——I 前缀 + 纯虚函数
class IScene
{
public:
    virtual ~IScene() = default;

    virtual void Initialize() = 0;
    virtual Entity CreateEntity() = 0;
    virtual void DestroyEntity(Entity we) = 0;
    virtual bool IsValid(Entity we) const = 0;
};
```

### 4.7 枚举规范

```cpp
// 枚举类型——E 前缀 + 大驼峰
enum class EMsgID : uint32
{
    MSG_HEARTBEAT_REQ = 1,
    MSG_HEARTBEAT_RSP = 2,
    MSG_MOVE_REQ = 200,
    MSG_MOVE_RSP = 201,
    MSG_ENTITY_CREATE_NTF = 400,
};

enum class ESceneType : uint8
{
    SCENE_PERSISTENT   = 0,  // 常驻场景
    SCENE_DUNGEON      = 1,  // 副本实例
    SCENE_RAID         = 2,  // 大型 Raid
    SCENE_BATTLEGROUND = 3,  // 战场
};
```

### 4.8 现代 C++ 特性

```cpp
// 三路比较运算符（C++20）
auto operator<=>(const Entity&) const = default;

// 约束模板（C++20 Concepts）
template <typename T>
concept IsComponent = requires(T t) {
    { T::kComponentName } -> std::convertible_to<const char*>;
};

// 指定初始化器（C++20）
PlayerRow row{
    .AccountID = 5001,
    .Name = "DarkMage",
    .ClassID = CLASS_MAGE,
    .Level = 1,
};

// std::optional 代替输出参数
std::optional<Entity> FindPlayer(uint32 accountID);

// std::expected (C++23) 代替异常
std::expected<Entity, ErrorCode> CreatePlayer(const std::string& name);
```

---

## 5. 错误处理规范

### 5.1 断言

```cpp
// MASSIVE_ASSERT — 自定义断言宏
// Debug: abort 进调试器
// Release: 记录 ERROR 日志后继续执行
MASSIVE_ASSERT(entity.IsValid(), "Entity must be valid before modifying components");
MASSIVE_ASSERT(queueDepth <= kMaxQueueSize, "Queue overflow detected");

// 绝对不要用标准 assert()——Release 下被完全移除
```

### 5.2 日志

```cpp
// 使用 MMO::Log —— spdlog 封装
// traceID 通过 thread_local 内置，入口处设置一次即可
#include "Common/Log/Log.h"
using namespace MMO;

// ==== 收包/任务入口 — 设置 traceID ====
Log::SetTraceID(msg.traceID);

// ==== 业务代码 — 零额外参数，source_location 自动捕获 file/line/func ====
Log::Error("DB connection lost: {}", e.what());
Log::Warn("Queue depth approaching limit: depth={} max={}", depth, kMaxQueueDepth);
Log::Info("Scene {} initialized: {} entities loaded", sceneID, entityCount);
Log::Debug("Entity {} Pos: ({:.2f}, {:.2f}, {:.2f})", entityID, x, y, z);

// ==== 输出格式 ====
// [2026-06-17 10:30:01.123] [I] [000001F3] [LoginHandler.cpp:78 OnAuth] Player logged in
//  时间戳             |  级别 | traceID   | file:line func              | message
```

| 级别 | API | 场景 |
|------|-----|------|
| ERROR | `Log::Error(...)` | DB断开、registry损坏（需立即响应） |
| WARN | `Log::Warn(...)` | 队列接近上限、认证失败、消息超时 |
| INFO | `Log::Info(...)` | 登录/登出、等级提升、交易、场景切换 |
| DEBUG | `Log::Debug(...)` | 组件值变化、AOI 事件、循环计数 |
| TRACE | `Log::Trace(...)` | 网络包内容 dump（仅本机开发） |
| CRITICAL | `Log::Critical(...)` | 内存耗尽、不可恢复的内部错误 |

> **注意**：忘记 SetTraceID 时 traceID 默认为 `kInvalidID`（0xFFFFFFFF），输出 `[FFFFFFFF]`。
> 不崩溃，但日志不可追踪——入口处必须显式设置 TraceScope 或 SetTraceID。

### 5.3 错误恢复策略

| 场景 | 策略 |
|------|------|
| C++ 引擎内部不变量 | `MASSIVE_ASSERT` → Debug abort / Release 继续 |
| DasLang 脚本逻辑错误 | `panic` → ERROR 日志 + 行号 → 返回错误给客户端 |
| DB 写入失败 | 重试 3 次（间隔 100ms）→ 仍失败则 ERROR 日志 + 继续服务 |
| Protobuf 解析失败 | ERROR 日志 + 跳过消息 + 不 crash |
| 内存耗尽 | 主动 `std::abort()` → core dump → systemd 自动重启 |
| EnTT registry 损坏 | panic → 全量 debug_dump → abort → 重启恢复 |

### 5.4 异常规范

- **不使用 C++ 异常**（与 Boost.Asio 默认设置一致）
- 用 `std::optional`、`std::expected`、错误码替代
- 仅在构造阶段可抛异常并在进程启动层捕获

---

## 6. ECS / EnTT 规范

### 6.1 Component 注册

```cpp
// C++ 高频组件——在 Module 构造函数中一行注册
// 禁止使用宏注册，必须用模板函数
ModuleEntt() : Module("ecs")
{
    ModuleLibrary lib(this);
    lib.addBuiltInModule();
    RegisterECSComponent<Position>(*this, lib, "Position");
    RegisterECSComponent<Velocity>(*this, lib, "Velocity");
    RegisterECSComponent<Health>(*this, lib, "Health");
    RegisterECSComponent<Mana>(*this, lib, "Mana");
}

// 高频瞬态组件——swap_and_pop 策略（无 tombstone 累积）
template <>
struct entt::component_traits<Position>
{
    static constexpr auto in_place_delete = false;
};
template <>
struct entt::component_traits<Velocity>
{
    static constexpr auto in_place_delete = false;
};
template <>
struct entt::component_traits<Health>
{
    static constexpr auto in_place_delete = false;
};

// 持久状态组件——保持 in_place（默认），定期 compact
// 无需特化 in_place_delete
```

### 6.2 Component 分类

| 类别 | 存储 | 删除策略 | 写入权 | 示例 |
|------|------|----------|--------|------|
| C++ 高频组件 | EnTT SoA | swap_and_pop | C++ System | Position, Velocity, Health |
| C++ 持久组件 | EnTT SoA | in_place + compact | C++ System | Inventory, SkillBar |
| 脚本组件 | BlobStorage SoA | swap_with_last O(1) | DasLang System | QuestProgress, BuffState |

### 6.3 ECS 操作

```cpp
// 遍历——用 owning group（保证 storage 对齐）
auto group = scene.EcsRegistry().group<entt::owned_t<Position, Velocity>>();
auto* posArr = group.storage<Position>().raw();
auto* velArr = group.storage<Velocity>().raw();
auto count = group.size();

// 单实体操作——通过 ComponentMap
auto& hp = registry.get<Health>(entity);
hp.current -= 10;

// 全局 Manager 不用 ECS——它们不是 entity 生命周期管理的对象
// ✅ std::unordered_map<uint32, Guild> _guilds;
// ❌ 不该把 guild 当 entity 注册到 registry
```

### 6.4 双缓冲模型

```cpp
// current = 只读快照，next = 写入目标
// 所有 ecs_query 读 current
// 所有 ecs_edit 写 next
// Tick 结束: swap(current, next)

// ⚠ 同一帧内 entity B 的计算不能依赖 entity A 在本 Tick 内的修改
// ⚠ ecs_query 不保证遍历顺序（EnTT view 反向迭代）
// 业务逻辑不应依赖 entity 的迭代先后
```

---

## 7. Protobuf / 网络协议规范

### 7.1 消息文件命名

```protobuf
// Proto/MsgMove.proto
syntax = "proto3";
package MMO;

// 请求必须有 _Req 后缀
message MoveReq
{
    uint32 session_id = 1;
    float x = 2;
    float y = 3;
    float z = 4;
}

// 响应必须有 _Rsp 后缀
message MoveRsp
{
    uint32 entity_id = 1;
    float corrected_x = 2;
    float corrected_y = 3;
}

// 推送必须有 _Ntf 后缀
message EntityCreateNtf
{
    uint32 entity_id = 1;
    uint32 template_id = 2;
    float x = 3;
    float y = 4;
}
```

### 7.2 proto 内部字段命名

- **必须使用** `snake_case` 字段名（protobuf 官方规范）
- C++ 生成的 `PascalCase` accessor 自动转换（`session_id` → `sessionID`）
- Protobuf package 名大写：`package MMO;`

```protobuf
uint32 session_id = 1;  // ✅ snake_case → C++ 端 sessionID
// uint32 sessionID = 1;  // ❌
```

### 7.3 Wire Format

```
PacketHeader: Length(4B) + MsgID(4B) + SessionID(4B) + Body
- Length: 大端，含头长度
- MsgID: EMsgID 枚举值，大端
- SessionID: Gate 分配，明文
- Body: 明文（认证阶段）或 AES-256-GCM 密文（业务阶段）
```

### 7.4 消息 ID 枚举定义

```protobuf
// Proto/MsgID.proto
enum EMsgID
{
    MSG_HEARTBEAT_REQ = 1;
    MSG_HEARTBEAT_RSP = 2;

    // 登录——走 LoginServer（TLS）
    MSG_LOGIN_AUTH_REQ = 100;
    MSG_LOGIN_AUTH_RSP = 101;

    // 进入世界——走 GateServer，携带 SessionToken（明文）
    MSG_LOGIN_ENTER_WORLD_REQ = 102;
    MSG_LOGIN_ENTER_WORLD_RSP = 103;

    // 业务消息
    MSG_MOVE_REQ = 200;
    MSG_MOVE_RSP = 201;
    MSG_SKILL_CAST_REQ = 300;
    MSG_SKILL_CAST_RSP = 301;
    MSG_SKILL_HIT_NTF = 302;
}
```

---

## 8. DasLang 脚本规范

### 8.1 文件组织

```dascript
// CombatSystem.das — 战斗系统：伤害管线、仇恨管理
// 所属: WorldServer
// 依赖: StatsCore.das, BuffSystem.das

require "StatsCore"
require "BuffSystem"

// 导出函数——使用 [export] 注解
[export]
def SystemCombat(sceneID: uint32; dt: float)
{
    // ...
}
```

### 8.2 命名规范

| 元素 | 规范 | 示例 |
|------|------|------|
| 脚本文件名 | 大驼峰 `.das` | `CombatSystem.das`, `ServerTick.das` |
| require 引用 | 大驼峰 | `require "StatsCore"` |
| 导出函数 | 大驼峰 | `ServerTick`, `SystemCombat` |
| 脚本组件 | 大驼峰 | `QuestProgress`, `BuffState` |
| 局部变量 | 小驼峰 | `entityID`, `damageDealt` |
| 脚本组件成员 | 小驼峰 | `questID`, `currentStage` |
| 函数参数 | 小驼峰 | `sceneID`, `deltaTime` |

### 8.3 Stage 组织

```dascript
// ServerTick.das — 顶层 Tick 调度
[export]
def ServerTick(sceneID: uint32; dt: float)
{
    ecs_stage "input"      { ProcessInput(sceneID) }
    ecs_stage "movement"   { SystemMovement(sceneID, dt) }
    ecs_stage "stats"      { SystemRecalcStats(sceneID) }
    ecs_stage "combat"     { SystemCombat(sceneID) }
    ecs_stage "buff"       { SystemBuffTick(sceneID, dt) }
    ecs_stage "ai"         { SystemAI(sceneID) }
    ecs_stage "quest"      { SystemQuest(sceneID) }
    ecs_stage "skill"      { SystemSkillCooldown(sceneID) }
    ecs_stage "aoi"        { SystemAOI(sceneID) }
    ecs_stage "replicate"  { SystemReplicate(sceneID) }
    ecs_stage "save"       { SystemDirtyFlush(sceneID) }
}
```

### 8.4 脚本组件定义

```dascript
// 用 [script_component] 注解标记脚本组件
[script_component]
struct QuestProgress
{
    questID: uint32
    currentStage: int32
    objectivesDone: array<uint32>
}

// 在 C++ 侧运行时注册：
// scene.RegisterScriptComponent("QuestProgress", sizeof(QuestProgress))
```

---

## 9. 数据库规范

### 9.1 表命名（SQL 层 snake_case）

| 元素 | 命名 | 示例 |
|------|------|------|
| 玩家数据表 | `players`, `player_*` | `player_inventory`, `player_skills` |
| 模板配置表 | `*_templates` | `item_templates`, `skill_templates` |
| 全局系统表 | 直接命名 | `guilds`, `accounts` |
| 系统表 | `schema_*` | `schema_migrations` |
| 列名 | snake_case | `player_id`, `last_login_at` |

### 9.2 C++ 层查询（自动生成的 Column 用 PascalCase + ID）

```cpp
// ✅ 类型安全查询——通过 DB::Range<T>
DB::Range<PlayersTable>()
    .Where(PlayersTable::PlayerID == 1001)
    .SingleOrDefault([](std::optional<PlayerRow> player)
    {
        if (player)
        {
            LoadPlayerIntoWorld(*player);
        }
    });

// ✅ 写入——指定初始化器保证字段遗漏编译报错
DB::Range<PlayersTable>().Insert(PlayerRow{
    .AccountID = 5001,
    .Name = "DarkMage",
    .ClassID = CLASS_MAGE,
    .Level = 1,
    .SceneID = 1001,
    .PosX = 100.0f,
    .PosY = 50.0f,
    .PosZ = 0.0f,
});

// ⚠ 复杂查询 only——不建议用于普通 CRUD
auto result = RuntimeQuery(
    "SELECT p.player_id, SUM(c.damage) as total_dps "
    "FROM players p JOIN combat_log c ON p.player_id = c.player_id "
    "WHERE p.guild_id = $1 GROUP BY p.player_id ORDER BY total_dps DESC LIMIT 10",
    {guildID}
);
```

### 9.3 自动生成文件命名

```cpp
// Src/Common/DB/AutoGen/PlayersTable.gen.h
// 由 Tools/generate_db_bindings.py 从 information_schema 自动生成
// DB 列名 player_id → C++ Column 名 PlayerID
struct PlayersTable
{
    static constexpr const char* kTableName = "players";

    static constexpr auto PlayerID    = Column<uint32>{"player_id",    kPK | kAutoInc};
    static constexpr auto AccountID   = Column<uint32>{"account_id",   kRequired};
    static constexpr auto ClassID     = Column<int32>{"class_id",      kRequired};
    static constexpr auto Level       = Column<int32>{"level",         kDefaulted, 1};
    static constexpr auto Gold        = Column<int64>{"gold",          kDefaulted, 0LL};
    static constexpr auto GuildID     = Column<uint32>{"guild_id",     kNullable};
    static constexpr auto SceneID     = Column<uint32>{"scene_id",     kRequired};
    // ...
};
```

---

## 10. 性能与可观测性规范

### 10.1 Tracy 埋点

```cpp
// 开发期性能追踪——Release 下编译为空白（零开销）
#ifdef MASSIVE_ENABLE_TRACY
  #include <tracy/Tracy.hpp>
  #define MASSIVE_PROFILE()       ZoneScoped
  #define MASSIVE_PROFILE_NAME(x) ZoneScopedN(x)
  #define MASSIVE_FRAME_MARK()    FrameMark
#else
  #define MASSIVE_PROFILE()       (void)0
  #define MASSIVE_PROFILE_NAME(x) (void)0
  #define MASSIVE_FRAME_MARK()    (void)0
#endif

void LogicServer::Run()
{
    while (!_stopped.load())
    {
        MASSIVE_FRAME_MARK();
        {
            MASSIVE_PROFILE_NAME("ProcessMessages");
            ProcessMessages();
        }
        {
            MASSIVE_PROFILE_NAME("GameTick");
            Tick(elapsed);
        }
    }
}
```

### 10.2 Metrics 指标命名

```cpp
// Prometheus 格式：snake_case
// massive_ 前缀（注意：metrics 名保持 snake_case 是 Prometheus 规范）
_metrics.RegisterGauge("massive_queue_depth",    [&] { return _queue.SizeApprox(); });
_metrics.RegisterGauge("massive_tick_duration_ms", [&] { return _lastTickMs.load(); });
_metrics.RegisterGauge("massive_sessions_online", [&] { return _sessionToEntity.size(); });
_metrics.IncrementCounter("massive_messages_processed_total");
```

---

## 11. 测试规范

### 11.1 测试三层金字塔

| 层级 | 位置 | 占比 | 内容 |
|------|------|------|------|
| **单元测试** | `Test/Unit/` | 60% | 各模块独立测试：队列、加密、Entity、定时器 |
| **集成测试** | `Test/Integration/` | 30% | 多模块协作：完整消息流、ECS 管线、DB 读写 |
| **性能基准** | `Test/Benchmark/` | 10% | BenchmarkScene 10000 entity 压测 |

### 11.2 测试文件与命名

```cpp
// Test/Unit/TestMpscQueue.cpp
TEST_CASE("MpscQueue::Enqueue single producer single consumer", "[queue]")
{
    // ...
}

TEST_CASE("MpscQueue::DrainAll returns all enqueued messages", "[queue]")
{
    // ...
}
```

---

## 12. 配套工具链

| 工具 | 用途 | 配置文件 |
|------|------|----------|
| **clang-format** | 自动格式化 | `.clang-format` |
| **clang-tidy** | 静态分析 + 命名检查 | `.clang-tidy` |
| **xmake** | 构建系统 | `xmake.lua` |
| **spdlog** | 日志库 | 代码内配置 |
| **Tracy** | 性能分析 | 编译宏 `MASSIVE_ENABLE_TRACY` |
| **Protobuf** | 序列化 | `Proto/*.proto` |
| **pg_bouncer** | 数据库连接池 | `pgbouncer.ini` |

---

> **版本**: 2.0
> **最后更新**: 2026-06-16
> **适用范围**: Massive MMO Server 全部 C++ / DasLang / Protobuf 代码
>
> **v2.0 变更摘要**：
> - 大括号 Allman 风格（开括号换行）
> - `xxxId` → `xxxID` 全局替换（含变量名、字段名、类型名）
> - 命名空间 `Mmo` → `MMO`, `Db` → `DB`
> - 类名中首字母缩写全大写（IO, DB, ECS, AOI, AI, RPC, GM...）
> - 文件命名统一大驼峰（.h/.cpp/.das/.proto/.toml）
> - 自动生成文件 `.gen.h/.gen.cpp`
> - 目录名统一大驼峰
> - Protobuf package 名大写 `MMO`
