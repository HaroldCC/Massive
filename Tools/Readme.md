# Tools — 构建 & 工具脚本

## 新机器初始化

```bash
# 第一步: 编译第三方依赖 (Windows: 自动使用本地 NASM)
python Tools/ThirdParty/BuildThirdParty.py

# 第二步: 编译项目
xmake
```

## 目录结构

```
Tools/
├── Readme.md
└── ThirdParty/
    ├── BuildThirdParty.py   ← 一键编译 OpenSSL / Protobuf / daScript / libpq
    ├── README.md            ← 详细构建说明
    └── windows/
        └── nasm/
            └── nasm.exe     ← NASM (Windows 64-bit, ~1.6MB)
```

## ThirdParty 分组

| 组 | 编译方式 | 库 |
|---|---------|---|
| **A 组** 外部脚本编译 | `BuildThirdParty.py` | OpenSSL, Protobuf, daScript, libpq |
| **B 组** xmake 内编译 | `ThirdParty/xmake.lua` → xmake target | Fmt, TracyClient |
| **C 组** 纯头文件 | `ThirdParty/xmake.lua` → headeronly | Asio, EnTT, Spdlog, ConcurrentQueue |

## 用法

```bash
# 编译全部第三方依赖 (增量编译，已构建的跳过)
python Tools/ThirdParty/BuildThirdParty.py

# 只编译部分
python Tools/ThirdParty/BuildThirdParty.py --target openssl protobuf

# 强制重建
python Tools/ThirdParty/BuildThirdParty.py --force

# 下载 Perl Portable (Windows)
python Tools/ThirdParty/BuildThirdParty.py --install-perl

# 跳过检查, 直接编译
python Tools/ThirdParty/BuildThirdParty.py --skip-check
```

## 警告策略

| 范围 | 策略 | 实现 |
|------|------|------|
| 项目代码 (Src/) | Warning as Error, `/W4` | `set_warnings("all", "error")` |
| B 组静态库 (Fmt/TracyClient) | 静默 | `set_warnings("none")` |
| C 组头文件 (Asio/EnTT/Spdlog/...) | 静默 | `add_sysincludedirs` |

## 产物

```
ThirdParty/
├── Bin/
│   ├── openssl/       (include/ + lib/)
│   ├── protobuf/      (include/ + lib/)
│   ├── dasScript/     (include/ + lib/)
│   └── libpq/         (include/ + lib/)
└── Build/
    ├── openssl/       (中间文件 + .done)
    ├── protobuf/      (中间文件 + .done)
    └── dasScript/     (中间文件 + .done)
```
