/**
 * @file EcdhX25519.h
 * @brief ECDH X25519 密钥协商 RAII 封装
 *
 * LoginServer: 生成 key pair → 接受客户端公钥 → DeriveSharedSecret → SessionKey
 * 使用 Curve25519（RFC 7748）
 */
#pragma once

#include <optional>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"

namespace MMO::Crypto
{

    /**
     * @brief X25519 密钥对
     */
    struct EcdhKeyPair
    {
        ByteBuffer privateKey; // 32B 私钥
        ByteBuffer publicKey;  // 32B 公钥
    };

    /**
     * @brief ECDH X25519 密钥协商
     */
    class EcdhX25519
    {
    public:
        /**
         * @brief 生成 X25519 密钥对
         * @return EcdhKeyPair，失败返回 nullopt
         */
        static std::optional<EcdhKeyPair> GenerateKeyPair();

        /**
         * @brief ECDH 计算共享密钥
         * @param privateKey    我们自己的私钥（32B）
         * @param peerPublicKey 对方的公钥（32B）
         * @return 32B shared secret，失败返回 nullopt
         */
        static std::optional<ByteBuffer> DeriveSharedSecret(const uint8 *privateKey,
                                                            const uint8 *peerPublicKey);

        /**
         * @brief sharedSecret → SHA-256 → 32B SessionKey
         * @param sharedSecret 共享密钥指针（32B）
         * @return 32B SessionKey
         */
        static ByteBuffer DeriveSessionKey(const uint8 *sharedSecret);

        static constexpr size_t kKeySize = 32; // X25519 密钥长度
    };

} // namespace MMO::Crypto
