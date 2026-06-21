# 基础设施 #5：Crypto 增强（argon2id）

> 状态：**设计已确认，待实现**
> 关联：[s7_protocol](../前期设计/architecture/s7_protocol.html)（LoginServer 密码验证）

## 1. 目标

在 `CommonCrypto` 增加 argon2id 密码哈希，供 LoginServer 的账户密码验证使用。
OpenSSL 3.x 通过 `EVP_KDF` 原生支持，零额外依赖。

## 2. API

```cpp
// Src/Common/Crypto/Argon2id.h
namespace MMO::Crypto
{

class Argon2id
{
public:
    // 哈希密码 → salt(16B) + hash(32B) = 48B hex 字符串
    static std::string HashPassword(std::string_view password);

    // 验证密码
    static bool VerifyPassword(std::string_view password, std::string_view storedHash);
};

}
```

## 3. 参数

按 OWASP 2023 推荐（用于认证场景）：

| 参数 | 值 |
|------|-----|
| iterations (t) | 2 |
| memory (m) | 64 MiB (65536 KiB) |
| parallelism (p) | 1 |
| hash length | 32 字节 |
| salt length | 16 字节（随机生成）|

存储格式：`salt(16B) + hash(32B)` → hex 编码（96 字符的 hex 字符串），直接存 DB 的 `accounts` 表。

## 4. 文件清单

```
Src/Common/Crypto/
├── Argon2id.h / .cpp       # argon2id 封装（OpenSSL EVP_KDF）
```

## 5. 未决/后续

- argon2id 是同步计算（~0.5s on 2 iterations/64MiB），LoginServer 的同步处理模型可接受。如需优化，可投递给线程池异步。
- 参数可配置化（迭代次数/内存从 server.toml 读），本期先硬编码 OWASP 推荐值。
