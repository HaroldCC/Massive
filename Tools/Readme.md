# Tools — 构建脚本

## 新机器初始化

```bash
# 第一步: 诊断环境 (不编译任何东西)
python Tools/Setup.py

# 第二步: 编译第三方依赖
python Tools/BuildThirdParty.py

# 第三步: 编译项目
xmake
```

## 脚本说明

| 脚本 | 用途 |
|------|------|
| `Setup.py` | 环境诊断，列出缺失工具 + 安装指引 |
| `BuildThirdParty.py` | 一键编译 ThirdParty 库 + 创建 xmake 壳文件 |
| `Toolchain.py` | 跨平台工具链发现 (被以上脚本导入) |

## ThirdParty 分组

| 组 | 编译方式 | 库 |
|---|---------|---|
| **A 组** 外部脚本编译 | `BuildThirdParty.py` | OpenSSL, Protobuf, libpq |
| **B 组** xmake 内编译 | `Build/xmake.lua` → xmake target | Fmt, TracyClient |
| **C 组** 纯头文件 | `add_sysincludedirs` | asio, entt, spdlog, concurrentqueue, daScript |

## 用法

```bash
# 全面诊断
python Tools/Setup.py

# 静默检查 (CI 用)
python Tools/Setup.py --check

# JSON 输出 (CI 集成)
python Tools/Setup.py --json

# 编译全部第三方依赖
python Tools/BuildThirdParty.py

# 只编译 OpenSSL, 清理重建
python Tools/BuildThirdParty.py --target openssl --clean

# 创建 xmake 壳文件 (fmt, tracy)
python Tools/BuildThirdParty.py --target fmt tracy

# 跳过检查, 直接编译
python Tools/BuildThirdParty.py --skip-check
```

## 警告策略

| 范围 | 策略 | 实现 |
|------|------|------|
| 项目代码 (Src/) | Warning as Error, `/W4` | `set_warnings("all", "error")` |
| B 组静态库 (Fmt/TracyClient) | 静默 | `set_warnings("none")` |
| C 组头文件 (asio/entt/spdlog/...) | 静默 | `add_sysincludedirs` |

## 产物

```
Build/
├── Fmt/
│   └── fmt.cpp              # B 组壳文件, xmake 编译为 libFmt.a
├── Tracy/
│   └── TracyClient.cpp      # B 组壳文件, xmake 编译为 libTracyClient.a
├── ThirdParty/
│   └── install/             # A 组产物
│       ├── openssl/
│       │   ├── include/  (146 headers)
│       │   └── lib/      (libcrypto, libssl)
│       ├── protobuf/
│       │   ├── include/  (627 headers)
│       │   └── lib/      (libprotobuf, libprotoc, absl_*)
│       └── libpq/ (待 PostgreSQL 安装)
└── xmake.lua                # B 组 xmake target 定义
```
