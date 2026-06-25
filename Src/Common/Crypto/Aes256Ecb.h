/**
 * @file Aes256Ecb.h
 * @brief AES-256-ECB 加解密（零 padding，单块）
 *
 * 仅用于 SessionToken 加密段：32B SessionKey → 32B 密文（恰好一个 AES-256 块）。
 * 不做 padding——调用方保证数据长度是 16B 的整数倍。
 */
#pragma once

#include <optional>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"

namespace MMO::Crypto
{

    /**
     * @brief AES-256-ECB 加解密（零 padding）
     *
     * 零 padding 实现，要求输入长度为 16B 的整数倍。
     * 专用于 SessionToken 加密段（恰好 32B）。
     */
    class Aes256Ecb
    {
    public:
        /**
         * @brief AES-256-ECB 加密（零 padding）
         * @param key        32B AES-256 密钥
         * @param plaintext  明文数据指针
         * @param len        数据长度（必须是 16B 的整数倍）
         * @return 密文（与明文等长），失败返回 nullopt
         */
        static std::optional<ByteBuffer> Encrypt(const uint8 *key, const uint8 *plaintext, size_t len);

        /**
         * @brief AES-256-ECB 解密（零 padding）
         * @param key         32B AES-256 密钥
         * @param ciphertext  密文数据指针
         * @param len         数据长度（必须是 16B 的整数倍）
         * @return 明文（与密文等长），失败返回 nullopt
         */
        static std::optional<ByteBuffer> Decrypt(const uint8 *key, const uint8 *ciphertext, size_t len);

        static constexpr size_t kKeySize   = 32; ///< AES-256
        static constexpr size_t kBlockSize = 16; ///< AES block
    };

} // namespace MMO::Crypto
