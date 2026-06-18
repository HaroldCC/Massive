# ThirdParty 构建工具集

本脚本编译需要 CMake 的第三方库（Protobuf、daScript）。
OpenSSL 和 libpq 的 Windows 二进制已 vendored 到 `ThirdParty/Bin/`，无需编译。

## 用法

```bash
# 编译全部（增量编译，已构建的跳过）
python Tools/ThirdParty/BuildThirdParty.py

# 只编译指定目标
python Tools/ThirdParty/BuildThirdParty.py --target protobuf

# 强制重建
python Tools/ThirdParty/BuildThirdParty.py --force
```

## 前置依赖

| 工具 | Windows | Linux |
|------|---------|-------|
| CMake | 手动安装 | `sudo apt install cmake` |
| libssl-dev | vendored | `sudo apt install libssl-dev` |
| libpq-dev | vendored | `sudo apt install libpq-dev` |

## Linux 用户

```bash
sudo apt install cmake libssl-dev libpq-dev
```
