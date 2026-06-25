/**
 * @file Argon2id.h
 * @brief argon2id 密码哈希（OpenSSL EVP_KDF 封装）
 *
 * OWASP 2023 推荐参数：2 iterations × 64 MiB memory × 1 parallelism。
 * 哈希输出格式：salt(16B) + hash(32B) → hex 编码（96 字符），直接存 DB。
 */
#pragma once

#include <string>
#include <string_view>

namespace MMO::Crypto
{

    class Argon2id
    {
    public:
        /**
         * @brief 哈希密码
         * @param password  明文密码
         * @return hex(salt + hash) = 96 字符的 hex 字符串
         */
        static std::string HashPassword(std::string_view password);

        /**
         * @brief 验证密码
         * @param password    明文密码
         * @param storedHash  HashPassword 输出的 96 字符 hex 字符串
         * @return 匹配返回 true
         */
        static bool VerifyPassword(std::string_view password, std::string_view storedHash);

    private:
        static constexpr int kSaltLen     = 16; // 字节
        static constexpr int kHashLen     = 32; // 字节
        static constexpr int kIterations  = 2;
        static constexpr int kMemoryKiB   = 65536; // 64 MiB
        static constexpr int kParallelism = 1;
    };

} // namespace MMO::Crypto
