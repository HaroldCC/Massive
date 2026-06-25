/**
 * @file CryptoSession.h
 * @brief per-session AES-256-GCM 加解密上下文
 *
 * WorldServer 侧，每登录成功的 sessionId 绑定一个 CryptoSession。
 * 负责：
 *   - 出站加密：sequence + clientRandom → 12B nonce → AES-256-GCM
 *   - 入站解密：序列号防重放 + AES-256-GCM 验证
 */
#pragma once

#include <cstdint>
#include <optional>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief per-session AES-256-GCM 加解密上下文
     *
     * 每个 sessionId 对应一个 CryptoSession。
     * 序列号用于防重放（每个方向独立递增）。
     */
    class CryptoSession
    {
    public:
        CryptoSession() = default;

        /**
         * @brief 用 SessionKey 和 clientRandom 初始化
         * @param sessionKey    32B AES-256 密钥
         * @param clientRandom  客户端随机前缀（nonce 下半部分）
         */
        void Init(const uint8 *sessionKey, uint64 clientRandom);

        // 32B AES-256 密钥
        const uint8 *SessionKey() const
        {
            return _sessionKey;
        }

        // 客户端随机前缀
        uint64 ClientRandom() const
        {
            return _clientRandom;
        }

        /**
         * @brief 出站加密
         * @param plaintext  明文数据
         * @param len        明文长度
         * @return ciphertext + 16B GCM tag 的 ByteBuffer，失败返回空 ByteBuffer
         */
        ByteBuffer Encrypt(const uint8 *plaintext, size_t len);

        /**
         * @brief 入站解密
         * @param ciphertext  密文 + 16B GCM tag 数据指针
         * @param len         密文总长度（含 tag）
         * @param seq         消息序列号
         * @return 明文的 ByteBuffer，验证失败 / 重放返回 nullopt
         */
        std::optional<ByteBuffer> Decrypt(const uint8 *ciphertext, size_t len, uint32 seq);

        /**
         * @brief 验证序列号是否有效（防重放）
         * @param seq  消息序列号
         * @return 有效返回 true
         */
        bool ValidateSequence(uint32 seq);

    private:
        void BuildNonce(uint32 sequence, uint8 *out) const;

        uint8  _sessionKey[32] = {}; // AES-256 密钥
        uint64 _clientRandom   = 0;  // 客户端随机前缀
        uint32 _lastSequence   = 0;  // 已收到的最大序列号
        uint32 _sendSequence   = 0;  // 已发送的序列号
    };

} // namespace MMO
