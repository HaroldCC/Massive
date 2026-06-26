# 基础设施 #6：DB 代码生成器

> 状态：**设计已确认，待实现**
> 关联：[s15_persistence](../前期设计/operations/s15_persistence.html)（Schema→C++ 代码生成）、[Range.h](../../Src/Common/DB/Range.h)（运行时底座）

## 1. 目标

从 SQL DDL 文件自动生成类型安全的 C++ TableSchema + RowType，使 C++ 层能以
`DB::Range<PlayersTable>().Where(PlayersTable::AccountID == 5001)` 方式做类型安全查询，
屏蔽手写 SQL 字符串。

## 2. 决策汇总

| 维度 | 选择 | 理由 |
|------|------|------|
| 真相来源 | **DDL 文件（`Tools/DB/SQL/*.sql`）** | 既能用 psql 建库建表，又无需运行中的 PG |
| SQL 解析 | **sqlparse（Python 库）** | AST 解析 CREATE TABLE，无需 PG 连接 |
| RowType | **全自动生成** | information_schema/DDL 有完整信息，手写会增加不同步风险 |
| 产物追踪 | **不追踪**（gitignore AutoGen/*.gen.h）| 和 Proto 的 .pb.h 策略一致，构建时生成 |
| 集成方式 | **xmake rule，自动增量** | 和 Protobuf 的 proto_gen 一样，每次 build 自动跑 |
| Timestamp | **`struct Timestamp`** | 强类型，后续可接 chrono 转换 |
| 命名风格 | **snake_case**（C++ 端和 PG 列名一致） | 减少概念映射，见 §3.4 |

## 3. 设计

### 3.1 输入：DDL 文件

```
Tools/DB/SQL/
├── 001_accounts.sql
├── 002_players.sql
├── 003_guilds.sql
└── ...
```

```sql
-- 001_accounts.sql
CREATE TABLE IF NOT EXISTS accounts
(
    account_id   SERIAL PRIMARY KEY,
    username     VARCHAR(255) NOT NULL UNIQUE,
    password_hash VARCHAR(96) NOT NULL,          -- argon2id hex(16B salt + 32B hash)
    email        VARCHAR(255),
    ban_until    TIMESTAMPTZ DEFAULT NULL,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_login_at TIMESTAMPTZ
);
```

### 3.2 输出：AutoGen header

生成到 `Src/Common/DB/AutoGen/`（gitignore）：

```cpp
/**
 * @file AccountsTable.gen.h
 * @brief 自动生成 —— PostgreSQL table 'accounts'
 *
 * 来源: Tools/DB/SQL/001_accounts.sql
 * 生成工具: Tools/DB/GenDBBindings.py
 * @warning 不要手动编辑
 */
#pragma once

#include "Common/DB/Column.h"
#include "Common/DB/Timestamp.h"

namespace MMO::DB::AutoGen
{

struct accounts_row
{
    int32_t    account_id    = 0;
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
    using RowType = accounts_row;
    using PKType  = int32_t;

    static constexpr auto account_id    = Column<int32_t>{"account_id",    kPK | kAutoInc};
    static constexpr auto username      = Column<std::string>{"username",    kRequired};
    static constexpr auto password_hash = Column<std::string>{"password_hash", kRequired};
    static constexpr auto email         = Column<std::string>{"email",       kNone};
    static constexpr auto ban_until     = Column<Timestamp>{"ban_until",     kNone};
    static constexpr auto created_at    = Column<Timestamp>{"created_at",    kRequired};
    static constexpr auto last_login_at = Column<Timestamp>{"last_login_at", kNone};
    static constexpr auto PK = account_id;
};

} // namespace MMO::DB::AutoGen
```

### 3.3 Timestamp 类型

```cpp
// Src/Common/DB/Timestamp.h
namespace MMO::DB
{

/**
 * @brief PostgreSQL TIMESTAMPTZ 对应的 C++ 强类型
 *
 * 内部以 int64_t 存储 Unix 毫秒时间戳。
 */
struct Timestamp
{
    int64_t unix_ms = 0;

    static Timestamp Now();
    static Timestamp FromUnix(int64_t ms);

    int64_t AsUnixMs() const { return unix_ms; }
};

} // namespace MMO::DB
```

### 3.4 命名约定

| 层 | 命名 | 示例 |
|----|------|------|
| SQL 列名 | `snake_case` | `account_id` |
| C++ Column | `snake_case`（和 SQL 一致） | `AccountsTable::account_id` |
| C++ Row 字段 | `snake_case`（和 SQL 一致） | `row.account_id` |
| C++ Table 结构体 | `PascalCase` + `Table` 后缀 | `AccountsTable` |
| Row 结构体 | `snake_case` + `_row` 后缀 | `accounts_row` |

**关键决策**：C++ 端沿用 PostgreSQL 的 `snake_case`，不做大小写转换。
理由：① 减少概念映射（SQL 列名 = C++ 字段名）② 避免大小写歧义（`accountID` vs `accountId`）③ protobuf 生成的 accessor 也是 snake_case 转小写，统一为 snake_case 减少认知负载。

### 3.5 类型映射

| PostgreSQL type | C++ type |
|-----------------|----------|
| `serial`, `integer`, `int4` | `int32_t` |
| `bigint`, `int8`, `bigserial` | `int64_t` |
| `smallint`, `int2` | `int16_t` |
| `real`, `float4` | `float` |
| `double precision`, `float8` | `double` |
| `boolean` | `bool` |
| `text`, `varchar`, `char` | `std::string` |
| `timestamp`, `timestamptz` | `Timestamp` |
| `bytea` | `std::vector<uint8_t>` |
| `jsonb` | `std::string` |
| `uuid` | `std::string` |

### 3.6 约束 → ColumnFlags 映射

| SQL 约束 | ColumnFlags |
|-----------|-------------|
| `PRIMARY KEY` | `kPK` |
| `SERIAL` / `BIGSERIAL` | `kAutoInc` |
| `NOT NULL` | `kRequired` |
| （无 NOT NULL 也无 DEFAULT） | `kNullable` |
| `DEFAULT <value>` | `kDefaulted` |

### 3.7 xmake 集成

```lua
-- Src/Common/DB/xmake.lua 新增 rule

rule("db_gen")
    on_config(function (target)
        local genScript = path.join(os.projectdir(), "Tools/DB/GenDBBindings.py")
        local sqlDir     = path.join(os.projectdir(), "Tools/DB/SQL")
        local outputDir  = path.join(os.projectdir(), "Src/Common/DB/AutoGen")

        -- 增量：DDL 文件比 .gen.h 新才重新生成
        local genFiles = os.files(path.join(outputDir, "*.gen.h"))
        local dirty = #genFiles == 0
        if not dirty then
            local oldestGen = os.mtime(genFiles[1])
            for _, f in ipairs(genFiles) do
                oldestGen = math.min(oldestGen, os.mtime(f))
            end
            for _, sqlF in ipairs(os.files(path.join(sqlDir, "*.sql"))) do
                if os.mtime(sqlF) > oldestGen then
                    dirty = true
                    break
                end
            end
        end

        if dirty then
            os.vrunv("python", {genScript, "--sql-dir", sqlDir, "--output", outputDir})
            cprint("${color.success}[db] .gen.h 已更新")
        end
    end)

target("CommonDB")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_headerfiles("AutoGen/*.gen.h")
    add_deps("CommonCore", "CommonQueue", "CommonLog", "LibPQ")
    add_rules("db_gen")
```

### 3.8 生成器工具

```
Tools/DB/
├── GenDBBindings.py          # 主脚本：扫 DDL → 生成 .gen.h
├── requirements.txt          # sqlparse（Python 依赖）
└── SQL/
    ├── 001_accounts.sql
    └── 002_players.sql
```

## 4. 文件清单

```
Tools/DB/
├── GenDBBindings.py          # 代码生成器
├── requirements.txt          # Python 依赖
└── SQL/                      # DDL 文件（git 追踪）
    └── *.sql

Src/Common/DB/
├── Timestamp.h               # Timestamp 强类型
├── AutoGen/                  # 产物（gitignore）
│   └── *.gen.h
└── (Column.h / Range.h ...)  # 已有
```

## 5. 使用示例

```cpp
#include "Common/DB/AutoGen/AccountsTable.gen.h"
#include "Common/DB/Range.h"

using namespace MMO::DB;
using namespace MMO::DB::AutoGen;

// 类型安全查询
DB::Range<AccountsTable>()
    .Where(AccountsTable::account_id == 5001)
    .SingleOrDefault([](std::optional<accounts_row> row) {
        if (row) {
            // row->username, row->password_hash 等直接可用
        }
    });
```

## 6. 变更记录 (2026-06-26)

### 改进点

#### DeserializeRow 按列名匹配

原设计按列索引 (`res.Get(i, 0)`) 反序列化，现改为按列名匹配。生成的 DeserializeRow 遍历 DBResult.Columns()，用 if-else 链匹配列名到 struct 字段，不依赖 SELECT 子句列顺序。

#### 三件套生成

每张表生成三个静态方法：
1. `DeserializeRow(res, rowIdx)` — 反序列化（Range::ToArray 调用）
2. `SerializeInsert(row)` → `(sql, params)` — INSERT（Range::Insert 调用）
3. `SerializeUpdateByPK(row)` → `(sql, params)` — UPDATE（Range::UpdateByPK 调用）

#### Timestamp 双向序列化

Timestamp 新增 `FromPGText(pgText)` 和 `ToPGText()` 方法，支持 PG TIMESTAMPTZ text 格式的解析和格式化。  
INSERT/UPDATE 中 Timestamp 列通过 `to_timestamp($N::bigint / 1000.0)` 写入，避免 libpq text 格式的时区歧义。

#### DBValue 支持 Timestamp

`DBValue` 新增 `DBValue(const Timestamp &)` 构造函数（在 Types.cpp 中实现，不内联）。

#### Range.h 修复

- `ToArray` 中反序列化空壳 → 调用 `Table::DeserializeRow(res, i)`
- `BuildInsertSingle` / `BuildInsertSQL` 空壳 → 调用 `Table::SerializeInsert(row)` 的 `InsertOne`
- 新增 `UpdateByPK(row)` 方法

#### players 表 DDL

新增 `Tools/DB/SQL/002_players.sql`，包含角色所需字段（account_id, name, level, class, gold, position 等）。
