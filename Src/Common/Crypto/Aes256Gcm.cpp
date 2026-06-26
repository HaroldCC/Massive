/**
 * @file Aes256Gcm.cpp
 * @brief AES-256-GCM 加解密实现
 */

#include "Common/Crypto/Aes256Gcm.h"
#include "Common/Core/MassiveAssert.h"

#include <openssl/err.h>
#include <openssl/evp.h>

namespace MMO::Crypto
{

    namespace
    {

        /**
 * @brief EVP_CIPHER_CTX RAII 封装
 */
        class EvpCipherCtx
        {
        public:
            EvpCipherCtx() : _ctx(EVP_CIPHER_CTX_new())
            {
            }

            ~EvpCipherCtx()
            {
                if (_ctx)
                {
                    EVP_CIPHER_CTX_free(_ctx);
                }
            }

            EvpCipherCtx(const EvpCipherCtx &)            = delete;
            EvpCipherCtx &operator=(const EvpCipherCtx &) = delete;

            EVP_CIPHER_CTX *Get()
            {
                return _ctx;
            }

        private:
            EVP_CIPHER_CTX *_ctx;
        };

    } // anonymous namespace

    /**
     * @brief AES-256-GCM 加密
     * @param key  32B AES-256 密钥
     * @param iv   12B nonce
     * @param plaintext    明文数据指针
     * @param plaintextLen 明文长度
     * @return ciphertext + 16B GCM tag 的 ByteBuffer，失败返回 nullopt
     */
    std::optional<ByteBuffer>
    Aes256Gcm::Encrypt(const uint8 *key, const uint8 *iv, const uint8 *plaintext, size_t plaintextLen)
    {
        EvpCipherCtx ctx;
        if (!ctx.Get())
        {
            return std::nullopt;
        }

        if (EVP_EncryptInit_ex(ctx.Get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        {
            return std::nullopt;
        }

        if (EVP_CIPHER_CTX_ctrl(ctx.Get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIvSize), nullptr) != 1)
        {
            return std::nullopt;
        }

        if (EVP_EncryptInit_ex(ctx.Get(), nullptr, nullptr, key, iv) != 1)
        {
            return std::nullopt;
        }

        auto out    = ByteBuffer::Own(plaintextLen + kTagSize);
        int  outLen = 0;

        if (EVP_EncryptUpdate(ctx.Get(), out.WritePtr(), &outLen, plaintext, static_cast<int>(plaintextLen))
            != 1)
        {
            return std::nullopt;
        }
        out.SetWritePos(outLen);

        int finalLen = 0;
        if (EVP_EncryptFinal_ex(ctx.Get(), out.WritePtr(), &finalLen) != 1)
        {
            return std::nullopt;
        }
        out.SetWritePos(outLen + finalLen);

        if (EVP_CIPHER_CTX_ctrl(ctx.Get(), EVP_CTRL_GCM_GET_TAG, kTagSize, out.WritePtr()) != 1)
        {
            return std::nullopt;
        }
        out.SetWritePos(outLen + finalLen + kTagSize);

        return out;
    }

    /**
     * @brief AES-256-GCM 解密并验证
     * @param key  32B AES-256 密钥
     * @param iv   12B nonce
     * @param ciphertext   密文 + 16B tag 数据指针
     * @param ciphertextLen 密文总长度（含 tag）
     * @return plaintext 的 ByteBuffer，验证失败返回 nullopt
     */
    std::optional<ByteBuffer>
    Aes256Gcm::Decrypt(const uint8 *key, const uint8 *iv, const uint8 *ciphertext, size_t ciphertextLen)
    {
        if (ciphertextLen < kTagSize)
        {
            return std::nullopt;
        }

        size_t       actualCipherLen = ciphertextLen - kTagSize;
        const uint8 *tag             = ciphertext + actualCipherLen;

        EvpCipherCtx ctx;
        if (!ctx.Get())
        {
            return std::nullopt;
        }

        if (EVP_DecryptInit_ex(ctx.Get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        {
            return std::nullopt;
        }

        if (EVP_CIPHER_CTX_ctrl(ctx.Get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIvSize), nullptr) != 1)
        {
            return std::nullopt;
        }

        if (EVP_DecryptInit_ex(ctx.Get(), nullptr, nullptr, key, iv) != 1)
        {
            return std::nullopt;
        }

        auto out    = ByteBuffer::Own(actualCipherLen);
        int  outLen = 0;

        if (EVP_DecryptUpdate(ctx.Get(),
                              out.WritePtr(),
                              &outLen,
                              ciphertext,
                              static_cast<int>(actualCipherLen))
            != 1)
        {
            return std::nullopt;
        }
        out.SetWritePos(outLen);

        if (EVP_CIPHER_CTX_ctrl(ctx.Get(), EVP_CTRL_GCM_SET_TAG, kTagSize, const_cast<uint8 *>(tag)) != 1)
        {
            return std::nullopt;
        }

        int finalLen = 0;
        if (EVP_DecryptFinal_ex(ctx.Get(), out.WritePtr(), &finalLen) != 1)
        {
            return std::nullopt;
        }
        out.SetWritePos(outLen + finalLen);

        return out;
    }

} // namespace MMO::Crypto
