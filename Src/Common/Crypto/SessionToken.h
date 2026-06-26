/**
 * @file SessionToken.h
 * @brief 46 字节自包含凭证
 *
 * [worldServerId: 2B] [accountId: 4B] [expireTime: 4B] [encryptedSessionKey: 32B] [HMAC: 4B]
 * ↑─── 明文段 10B ───↑   ↑─── 加密段 32B ───↑  ↑─HMAC─↑
 *
 * GateServer 只读 [0..1] → worldServerId 路由，无需解密。
 * WorldServer 验证 HMAC + 解密 SessionKey。
 */
#pragma once

#include <cstring>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"

namespace MMO::Crypto
{

    /**
     * @brief SessionToken — 可拷贝、可序列化的凭证
     */
    struct SessionToken
    {
        static constexpr size_t kTotalSize  = 46; // 总大小
        static constexpr size_t kPlainSize  = 10; // worldServerId(2B) + accountId(4B) + expireTime(4B)
        static constexpr size_t kKeyEncSize = 32; // AES-256-ECB 加密的 SessionKey
        static constexpr size_t kHmacSize   = 4;  // HMAC-SHA256 截断

        uint8 data[kTotalSize] = {};

        // ── 明文段读取 ──

        /**
 * @brief 读取 WorldServerId
 */
        uint16 WorldServerId() const
        {
            uint16 id;
            std::memcpy(&id, data, 2);
            return id;
        }

        /**
 * @brief 读取 AccountId
 */
        uint32 AccountId() const
        {
            uint32 id;
            std::memcpy(&id, data + 2, 4);
            return id;
        }

        /**
 * @brief 读取过期时间
 */
        uint32 ExpireTime() const
        {
            uint32 t;
            std::memcpy(&t, data + 6, 4);
            return t;
        }

        // ── 明文段写入（LoginServer 签发时内部使用）──

        /**
 * @brief 设置 WorldServerId
 */
        void SetWorldServerId(uint16 id)
        {
            std::memcpy(data, &id, 2);
        }

        /**
 * @brief 设置 AccountId
 */
        void SetAccountId(uint32 id)
        {
            std::memcpy(data + 2, &id, 4);
        }

        /**
 * @brief 设置过期时间
 */
        void SetExpireTime(uint32 t)
        {
            std::memcpy(data + 6, &t, 4);
        }

        // ── 加密段 ──

        const uint8 *EncryptedSessionKey() const
        {
            return data + kPlainSize;
        }

        uint8 *EncryptedSessionKey()
        {
            return data + kPlainSize;
        }

        // ── HMAC 段 ──

        const uint8 *Hmac() const
        {
            return data + kPlainSize + kKeyEncSize;
        }

        uint8 *Hmac()
        {
            return data + kPlainSize + kKeyEncSize;
        }

        // ── 序列化 ──

        /**
 * @brief 序列化为 ByteBuffer
 */
        ByteBuffer ToBuffer() const
        {
            return ByteBuffer::Copy(data, kTotalSize);
        }

        /**
         * @brief 从缓冲区反序列化
         * @param buf  数据指针
         * @param len  数据长度
         * @return SessionToken，长度不匹配返回 nullopt
         */
        static std::optional<SessionToken> FromBuffer(const uint8 *buf, size_t len)
        {
            if (len != kTotalSize)
            {
                return std::nullopt;
            }

            SessionToken token;
            std::memcpy(token.data, buf, kTotalSize);
            return token;
        }
    };

    /**
     * @brief SessionTokenBuilder — LoginServer 签发 SessionToken
     */
    class SessionTokenBuilder
    {
    public:
        /**
         * @brief 签发 SessionToken
         * @param lss             LoginServerSecret (32B, 与 WorldServer 共享)
         * @param sessionKey      ECDH 产生的 32B SessionKey
         * @param worldServerId   目标 WorldServer ID
         * @param accountId       玩家账号 ID
         * @param expireTime      过期时间戳
         * @return SessionToken，失败返回 nullopt
         */
        static std::optional<SessionToken> Issue(const uint8 *lss,
                                                 const uint8 *sessionKey,
                                                 uint16       worldServerId,
                                                 uint32       accountId,
                                                 uint32       expireTime);

        /**
 * @brief Verify 的解密结果
 */
        struct TokenPayload
        {
            uint32     accountId;  // 玩家账号 ID
            uint32     expireTime; // 过期时间戳
            ByteBuffer sessionKey; // 32B SessionKey
        };

        /**
         * @brief WorldServer 验证 SessionToken
         * @param lss    LoginServerSecret (32B)
         * @param token  待验证的 SessionToken
         * @return TokenPayload，验证失败返回 nullopt
         */
        static std::optional<TokenPayload> Verify(const uint8 *lss, const SessionToken &token);
    };

} // namespace MMO::Crypto
