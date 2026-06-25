/**
 * @file Argon2id.cpp
 * @brief argon2id 实现（OpenSSL EVP_KDF）
 */

#include "Common/Crypto/Argon2id.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <cstring>
#include <sstream>

namespace MMO::Crypto
{

    // OpenSSL 3.x argon2id 参数名
    static const char *kNameArg2 = "argon2id";
    static const char *kSalt     = OSSL_KDF_PARAM_SALT;
    static const char *kPass     = OSSL_KDF_PARAM_PASSWORD;
    static const char *kIter     = OSSL_KDF_PARAM_ITER;
    static const char *kMem      = OSSL_KDF_PARAM_ARGON2_MEMCOST;
    static const char *kLanes    = OSSL_KDF_PARAM_ARGON2_LANES;

    std::string Argon2id::HashPassword(std::string_view password)
    {
        // 1. 生成随机 salt
        unsigned char salt[kSaltLen];
        RAND_bytes(salt, kSaltLen);

        // 2. 调用 EVP_KDF argon2id
        unsigned char out[kHashLen];
        {
            EVP_KDF *kdf = EVP_KDF_fetch(nullptr, kNameArg2, nullptr);
            if (!kdf)
            {
                return "";
            }

            EVP_KDF_CTX *ctx = EVP_KDF_CTX_new(kdf);
            EVP_KDF_free(kdf);

            int      iterations = kIterations;
            int      p          = kParallelism;
            uint64_t mem        = kMemoryKiB;

            OSSL_PARAM params[] = {OSSL_PARAM_construct_octet_string(kPass,
                                                                     const_cast<char *>(password.data()),
                                                                     password.size()),
                                   OSSL_PARAM_construct_octet_string(kSalt, salt, kSaltLen),
                                   OSSL_PARAM_construct_int(kIter, &iterations),
                                   OSSL_PARAM_construct_uint64(kMem, &mem),
                                   OSSL_PARAM_construct_int(kLanes, &p),
                                   OSSL_PARAM_construct_end()};

            bool ok = EVP_KDF_derive(ctx, out, kHashLen, params);
            EVP_KDF_CTX_free(ctx);
            if (!ok)
            {
                return "";
            }
        }

        // 3. 构造 salt + hash → hex
        std::ostringstream hex;
        hex << std::hex;
        for (int i = 0; i < kSaltLen; ++i)
        {
            hex << (salt[i] >> 4) << (salt[i] & 0x0F);
        }
        for (int i = 0; i < kHashLen; ++i)
        {
            hex << (out[i] >> 4) << (out[i] & 0x0F);
        }

        return hex.str();
    }

    static int HexDigit(char c)
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return 0;
    }

    bool Argon2id::VerifyPassword(std::string_view password, std::string_view storedHash)
    {
        // 解析 hex 字符串：前 kSaltLen*2 字符 = salt，后 kHashLen*2 = hash
        const size_t expectedLen = (kSaltLen + kHashLen) * 2;
        if (storedHash.size() != expectedLen)
        {
            return false;
        }

        unsigned char salt[kSaltLen];
        unsigned char expected[kHashLen];

        for (int i = 0; i < kSaltLen; ++i)
        {
            salt[i] = static_cast<unsigned char>((HexDigit(storedHash[i * 2]) << 4)
                                                 | HexDigit(storedHash[i * 2 + 1]));
        }

        for (int i = 0; i < kHashLen; ++i)
        {
            size_t pos = (static_cast<size_t>(kSaltLen) + i) * 2;
            expected[i] =
                static_cast<unsigned char>((HexDigit(storedHash[pos]) << 4) | HexDigit(storedHash[pos + 1]));
        }

        // 用给定的 salt 重新 hash，比对结果
        unsigned char computed[kHashLen];
        {
            EVP_KDF *kdf = EVP_KDF_fetch(nullptr, kNameArg2, nullptr);
            if (!kdf)
            {
                return false;
            }

            EVP_KDF_CTX *ctx = EVP_KDF_CTX_new(kdf);
            EVP_KDF_free(kdf);

            int      iterations = kIterations;
            int      p          = kParallelism;
            uint64_t mem        = kMemoryKiB;

            OSSL_PARAM params[] = {OSSL_PARAM_construct_octet_string(kPass,
                                                                     const_cast<char *>(password.data()),
                                                                     password.size()),
                                   OSSL_PARAM_construct_octet_string(kSalt, salt, kSaltLen),
                                   OSSL_PARAM_construct_int(kIter, &iterations),
                                   OSSL_PARAM_construct_uint64(kMem, &mem),
                                   OSSL_PARAM_construct_int(kLanes, &p),
                                   OSSL_PARAM_construct_end()};

            bool ok = EVP_KDF_derive(ctx, computed, kHashLen, params);
            EVP_KDF_CTX_free(ctx);
            if (!ok)
            {
                return false;
            }
        }

        return std::memcmp(expected, computed, kHashLen) == 0;
    }

} // namespace MMO::Crypto
