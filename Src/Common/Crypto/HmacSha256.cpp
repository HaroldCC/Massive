/**
 * @file HmacSha256.cpp
 * @brief HMAC-SHA256 实现
 */

#include "Common/Crypto/HmacSha256.h"

#include <openssl/hmac.h>

namespace MMO::Crypto
{

/**
 * @brief HMAC-SHA256(key, data) → 32B
 * @param key     密钥指针
 * @param keyLen  密钥长度
 * @param data    数据指针
 * @param dataLen 数据长度
 * @return 32B HMAC，失败返回 nullopt
 */
std::optional<ByteBuffer> HmacSha256::Sign(
    const uint8* key,
    size_t keyLen,
    const uint8* data,
    size_t dataLen)
{
    auto out = ByteBuffer::Own(kHashSize);

    unsigned int outLen = 0;
    uint8* result = HMAC(
        EVP_sha256(),
        key,
        static_cast<int>(keyLen),
        data,
        dataLen,
        out.Data(),
        &outLen);

    if (!result || outLen != kHashSize)
    {
        return std::nullopt;
    }

    out.SetWritePos(kHashSize);
    return out;
}

} // namespace MMO::Crypto
