/**
 * @file CryptoSession.h
 * @brief per-session AES-256-GCM 加解密上下文
 *
 * WorldServer 侧，每登录成功的 sessionId 绑定一个 CryptoSession。
 * 负责：
 *   - 出站加密：sequence + clientRandom → 12B nonce → AES-256-GCM
 *   - 入站解密：序列号防重放 + AES-256-GCM 验证
 *
 * ── 重连安全 ──
 * 断线重连时 TCP 断开但 CryptoSession 保留。客户端在新的 EnterWorldReq
 * 中携带 reconnectSeed，服务端调用 RotateReconnectKey() 派生新密钥 + 新
 * clientRandom，序列号从 0 开始。新 nonce 空间不与旧 nonce 空间碰撞。
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

        /**
         * @brief 重连密钥旋转——派生新 key + newRandom，序列号归零
         *
         * 新密钥   = HMAC-SHA256(oldKey, "massive-reconnect-key-v1" || reconnectSeed)
         * 新随机数 = HMAC-SHA256(oldKey, "massive-reconnect-rand-v1" || reconnectSeed)[0..7]
         *
         * @param reconnectSeed  客户端提供的重连种子
         * @param seedLen        种子长度
         */
        void RotateReconnectKey(const uint8 *reconnectSeed, size_t seedLen);

        const uint8 *SessionKey() const { return _sessionKey; }
        uint64       ClientRandom() const { return _clientRandom; }

        /**
         * @brief 出站加密
         * @param plaintext  明文数据
         * @param len        明文长度
         * @return ciphertext + 16B GCM tag 的 ByteBuffer，失败返回空
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

        bool ValidateSequence(uint32 seq);

    private:
        void BuildNonce(uint32 sequence, uint8 *out) const;

        uint8  _sessionKey[32] = {};
        uint64 _clientRandom   = 0;
        uint32 _lastSequence   = 0;
        uint32 _sendSequence   = 0;
    };

} // namespace MMO
