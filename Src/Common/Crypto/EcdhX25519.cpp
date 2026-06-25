/**
 * @file EcdhX25519.cpp
 * @brief ECDH X25519 加解密实现
 */

#include "Common/Crypto/EcdhX25519.h"
#include "Common/Core/MassiveAssert.h"

#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <cstring>

namespace MMO::Crypto
{

    /**
     * @brief 生成 X25519 密钥对
     * @return EcdhKeyPair，失败返回 nullopt
     */
    std::optional<EcdhKeyPair> EcdhX25519::GenerateKeyPair()
    {
        EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
        if (!pctx)
        {
            return std::nullopt;
        }

        struct PctxGuard
        {
            EVP_PKEY_CTX *ctx;

            ~PctxGuard()
            {
                if (ctx)
                {
                    EVP_PKEY_CTX_free(ctx);
                }
            }
        } pctxGuard {pctx};

        if (EVP_PKEY_keygen_init(pctx) != 1)
        {
            return std::nullopt;
        }

        EVP_PKEY *pkey = nullptr;
        if (EVP_PKEY_keygen(pctx, &pkey) != 1)
        {
            return std::nullopt;
        }

        struct PkeyGuard
        {
            EVP_PKEY *key;

            ~PkeyGuard()
            {
                if (key)
                {
                    EVP_PKEY_free(key);
                }
            }
        } pkeyGuard {pkey};

        EcdhKeyPair result;

        size_t privLen = kKeySize;
        auto   privBuf = ByteBuffer::Own(kKeySize);
        if (EVP_PKEY_get_raw_private_key(pkey, privBuf.Data(), &privLen) != 1 || privLen != kKeySize)
        {
            return std::nullopt;
        }
        privBuf.SetWritePos(kKeySize);
        result.privateKey = std::move(privBuf);

        size_t pubLen = kKeySize;
        auto   pubBuf = ByteBuffer::Own(kKeySize);
        if (EVP_PKEY_get_raw_public_key(pkey, pubBuf.Data(), &pubLen) != 1 || pubLen != kKeySize)
        {
            return std::nullopt;
        }
        pubBuf.SetWritePos(kKeySize);
        result.publicKey = std::move(pubBuf);

        return result;
    }

    /**
     * @brief ECDH 计算共享密钥
     * @param privateKey    我们自己的私钥（32B）
     * @param peerPublicKey 对方的公钥（32B）
     * @return 32B shared secret，失败返回 nullopt
     */
    std::optional<ByteBuffer> EcdhX25519::DeriveSharedSecret(const uint8 *privateKey,
                                                             const uint8 *peerPublicKey)
    {
        EVP_PKEY *privKey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, privateKey, kKeySize);
        if (!privKey)
        {
            return std::nullopt;
        }

        struct PrivGuard
        {
            EVP_PKEY *key;

            ~PrivGuard()
            {
                if (key)
                {
                    EVP_PKEY_free(key);
                }
            }
        } privGuard {privKey};

        EVP_PKEY *pubKey = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peerPublicKey, kKeySize);
        if (!pubKey)
        {
            return std::nullopt;
        }

        struct PubGuard
        {
            EVP_PKEY *key;

            ~PubGuard()
            {
                if (key)
                {
                    EVP_PKEY_free(key);
                }
            }
        } pubGuard {pubKey};

        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(privKey, nullptr);
        if (!ctx)
        {
            return std::nullopt;
        }

        struct CtxGuard
        {
            EVP_PKEY_CTX *ctx;

            ~CtxGuard()
            {
                if (ctx)
                {
                    EVP_PKEY_CTX_free(ctx);
                }
            }
        } ctxGuard {ctx};

        if (EVP_PKEY_derive_init(ctx) != 1)
        {
            return std::nullopt;
        }

        if (EVP_PKEY_derive_set_peer(ctx, pubKey) != 1)
        {
            return std::nullopt;
        }

        size_t secretLen = 0;
        if (EVP_PKEY_derive(ctx, nullptr, &secretLen) != 1)
        {
            return std::nullopt;
        }

        auto secret = ByteBuffer::Own(secretLen);
        if (EVP_PKEY_derive(ctx, secret.Data(), &secretLen) != 1)
        {
            return std::nullopt;
        }
        secret.SetWritePos(secretLen);

        return secret;
    }

    /**
     * @brief sharedSecret → SHA-256 → 32B SessionKey
     * @param sharedSecret 共享密钥指针（32B）
     * @return 32B SessionKey
     */
    ByteBuffer EcdhX25519::DeriveSessionKey(const uint8 *sharedSecret)
    {
        auto sessionKey = ByteBuffer::Own(kKeySize);

        SHA256(sharedSecret, kKeySize, sessionKey.Data());
        sessionKey.SetWritePos(kKeySize);

        return sessionKey;
    }

} // namespace MMO::Crypto
