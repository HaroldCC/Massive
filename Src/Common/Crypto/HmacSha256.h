/**
 * @file HmacSha256.h
 * @brief HMAC-SHA256 RAII 封装
 *
 * 用于 SessionToken 签名：HMAC-SHA256(LSS, token[0..41]) → 取前 4B
 */
#pragma once

#include <optional>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"

namespace MMO::Crypto
{

    /**
     * @brief HMAC-SHA256 签名
     */
    class HmacSha256
    {
    public:
        /**
         * @brief HMAC-SHA256(key, data) → 32B
         * @param key     密钥指针
         * @param keyLen  密钥长度
         * @param data    数据指针
         * @param dataLen 数据长度
         * @return 32B HMAC，失败返回 nullopt
         */
        static std::optional<ByteBuffer>
        Sign(const uint8 *key, size_t keyLen, const uint8 *data, size_t dataLen);

        static constexpr size_t kHashSize = 32; ///< SHA-256 output
    };

} // namespace MMO::Crypto
