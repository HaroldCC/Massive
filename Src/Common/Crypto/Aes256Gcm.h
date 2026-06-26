/**
 * @file Aes256Gcm.h
 * @brief AES-256-GCM 加解密 RAII 封装
 *
 * 用于客户端 ↔ WorldServer 业务消息加解密。
 * Nonce: sequence(4B) + clientRandom(8B) = 12B
 * GCM Tag: 16B 自动追加/验证
 */
#pragma once

#include <optional>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"

namespace MMO::Crypto
{

    /**
     * @brief AES-256-GCM 加解密
     *
     * 返回 ciphertext + 16B GCM tag 组合的 ByteBuffer（Own 模式）。
     * Decrypt 时自动从密文末尾提取 tag 并验证。
     */
    class Aes256Gcm
    {
    public:
        /**
         * @brief AES-256-GCM 加密
         * @param key  32B AES-256 密钥
         * @param iv   12B nonce
         * @param plaintext    明文数据指针
         * @param plaintextLen 明文长度
         * @return ciphertext + 16B GCM tag 的 ByteBuffer，失败返回 nullopt
         */
        static std::optional<ByteBuffer>
        Encrypt(const uint8 *key, const uint8 *iv, const uint8 *plaintext, size_t plaintextLen);

        /**
         * @brief AES-256-GCM 解密并验证
         * @param key  32B AES-256 密钥
         * @param iv   12B nonce
         * @param ciphertext   密文 + 16B tag 数据指针
         * @param ciphertextLen 密文总长度（含 tag）
         * @return plaintext 的 ByteBuffer，验证失败返回 nullopt
         */
        static std::optional<ByteBuffer>
        Decrypt(const uint8 *key, const uint8 *iv, const uint8 *ciphertext, size_t ciphertextLen);

        static constexpr size_t kKeySize = 32; // AES-256
        static constexpr size_t kIvSize  = 12; // GCM standard nonce
        static constexpr size_t kTagSize = 16; // GCM authentication tag
    };

} // namespace MMO::Crypto
