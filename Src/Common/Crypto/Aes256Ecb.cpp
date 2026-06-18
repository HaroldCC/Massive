/**
 * @file Aes256Ecb.cpp
 * @brief AES-256-ECB 加解密实现（零 padding）
 */

#include "Common/Crypto/Aes256Ecb.h"
#include "Common/Core/MassiveAssert.h"

#include <openssl/evp.h>

namespace MMO::Crypto
{

namespace
{

/** @brief EVP_CIPHER_CTX RAII 封装 */
class EvpCipherCtx
{
public:
    EvpCipherCtx()
        : _ctx(EVP_CIPHER_CTX_new())
    {
    }

    ~EvpCipherCtx()
    {
        if (_ctx)
        {
            EVP_CIPHER_CTX_free(_ctx);
        }
    }

    EvpCipherCtx(const EvpCipherCtx&) = delete;
    EvpCipherCtx& operator=(const EvpCipherCtx&) = delete;

    EVP_CIPHER_CTX* Get() { return _ctx; }

private:
    EVP_CIPHER_CTX* _ctx;
};

} // anonymous namespace

/**
 * @brief AES-256-ECB 加密（零 padding）
 * @param key        32B AES-256 密钥
 * @param plaintext  明文数据指针
 * @param len        数据长度（必须是 16B 的整数倍）
 * @return 密文（与明文等长），失败返回 nullopt
 */
std::optional<ByteBuffer> Aes256Ecb::Encrypt(
    const uint8* key,
    const uint8* plaintext,
    size_t len)
{
    MASSIVE_ASSERT(len % kBlockSize == 0,
        "Aes256Ecb::Encrypt: data length must be multiple of 16");

    EvpCipherCtx ctx;
    if (!ctx.Get())
    {
        return std::nullopt;
    }

    if (EVP_EncryptInit_ex(ctx.Get(), EVP_aes_256_ecb(), nullptr, key, nullptr) != 1)
    {
        return std::nullopt;
    }
    EVP_CIPHER_CTX_set_padding(ctx.Get(), 0);

    auto out = ByteBuffer::Own(len);
    int outLen = 0;

    if (EVP_EncryptUpdate(ctx.Get(), out.WritePtr(), &outLen, plaintext, static_cast<int>(len)) != 1)
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

    return out;
}

/**
 * @brief AES-256-ECB 解密（零 padding）
 * @param key         32B AES-256 密钥
 * @param ciphertext  密文数据指针
 * @param len         数据长度（必须是 16B 的整数倍）
 * @return 明文（与密文等长），失败返回 nullopt
 */
std::optional<ByteBuffer> Aes256Ecb::Decrypt(
    const uint8* key,
    const uint8* ciphertext,
    size_t len)
{
    MASSIVE_ASSERT(len % kBlockSize == 0,
        "Aes256Ecb::Decrypt: data length must be multiple of 16");

    EvpCipherCtx ctx;
    if (!ctx.Get())
    {
        return std::nullopt;
    }

    if (EVP_DecryptInit_ex(ctx.Get(), EVP_aes_256_ecb(), nullptr, key, nullptr) != 1)
    {
        return std::nullopt;
    }
    EVP_CIPHER_CTX_set_padding(ctx.Get(), 0);

    auto out = ByteBuffer::Own(len);
    int outLen = 0;

    if (EVP_DecryptUpdate(ctx.Get(), out.WritePtr(), &outLen, ciphertext, static_cast<int>(len)) != 1)
    {
        return std::nullopt;
    }
    out.SetWritePos(outLen);

    int finalLen = 0;
    if (EVP_DecryptFinal_ex(ctx.Get(), out.WritePtr(), &finalLen) != 1)
    {
        return std::nullopt;
    }
    out.SetWritePos(outLen + finalLen);

    return out;
}

} // namespace MMO::Crypto
