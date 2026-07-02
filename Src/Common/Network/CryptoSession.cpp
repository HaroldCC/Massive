/**
 * @file CryptoSession.cpp
 * @brief per-session AES-256-GCM 加解密上下文实现
 */

#include "Common/Network/CryptoSession.h"
#include "Common/Network/PacketHeader.h"
#include "Common/Crypto/Aes256Gcm.h"
#include "Common/Crypto/HmacSha256.h"
#include "Common/Log/Log.h"

#include <cstring>

namespace MMO
{

    void CryptoSession::Init(const uint8 *sessionKey, uint64 clientRandom)
    {
        std::memcpy(_sessionKey, sessionKey, 32);
        _clientRandom = clientRandom;
        _lastSequence = 0;
        _sendSequence = 0;
    }

    void CryptoSession::RotateReconnectKey(const uint8 *reconnectSeed, size_t seedLen)
    {
        // 先保存旧 key——两个派生都用同一个旧 key，避免第一个派生改了 _sessionKey 影响第二个
        uint8 oldKey[32];
        std::memcpy(oldKey, _sessionKey, 32);

        constexpr char kKeyDomain[]  = "massive-reconnect-key-v1";
        constexpr char kRandDomain[] = "massive-reconnect-rand-v1";
        uint8          combined[64 + sizeof(kKeyDomain)];
        size_t         copyLen = seedLen;
        if (seedLen > 64)
        {
            Log::Warn("CryptoSession: reconnect seed too large ({} > 64), truncating", seedLen);
            copyLen = 64;
        }

        // 派生新密钥
        std::memcpy(combined, kKeyDomain, sizeof(kKeyDomain));
        std::memcpy(combined + sizeof(kKeyDomain), reconnectSeed, copyLen);
        auto newKey = Crypto::HmacSha256::Sign(oldKey, 32, combined, sizeof(kKeyDomain) + copyLen);
        if (newKey && newKey->Size() == 32)
        {
            std::memcpy(_sessionKey, newKey->Data(), 32);
        }

        // 派生新 clientRandom（用同一个旧 key）
        std::memcpy(combined, kRandDomain, sizeof(kRandDomain));
        std::memcpy(combined + sizeof(kRandDomain), reconnectSeed, copyLen);
        auto newRandom = Crypto::HmacSha256::Sign(oldKey, 32, combined, sizeof(kRandDomain) + copyLen);
        if (newRandom && newRandom->Size() >= 8)
        {
            uint64 randVal = 0;
            std::memcpy(&randVal, newRandom->Data(), 8);
            _clientRandom = randVal;
        }

        _lastSequence = 0;
        _sendSequence = 0;
    }

    ByteBuffer CryptoSession::Encrypt(const uint8 *plaintext, size_t len)
    {
        uint32 seq = ++_sendSequence;

        uint8 nonce[12];
        BuildNonce(seq, nonce);

        auto cipherResult = Crypto::Aes256Gcm::Encrypt(_sessionKey, nonce, plaintext, len);
        if (!cipherResult)
        {
            return ByteBuffer {};
        }

        // 返回值 = [Seq:4B][ciphertext + 16B GCM tag]
        auto out = ByteBuffer::Own(sizeof(uint32) + cipherResult->Size());
        out.WriteUint32(seq);
        out.WriteBytes(cipherResult->Data(), cipherResult->Size());
        return out;
    }

    std::optional<ByteBuffer> CryptoSession::Decrypt(const uint8 *ciphertext, size_t len, uint32 seq)
    {
        if (!ValidateSequence(seq))
        {
            return std::nullopt;
        }

        uint8 nonce[12];
        BuildNonce(seq, nonce);

        auto result = Crypto::Aes256Gcm::Decrypt(_sessionKey, nonce, ciphertext, len);
        if (!result)
        {
            return std::nullopt;
        }

        _lastSequence = seq;
        return std::move(*result);
    }

    bool CryptoSession::ValidateSequence(uint32 seq)
    {
        // 序列号回绕处理：允许 seq 比 lastSequence 最多小 2^31
        constexpr uint32 kHalfRange = 0x80000000u;
        uint32           diff       = seq - _lastSequence;
        return diff > 0 && diff < kHalfRange;
    }

    void CryptoSession::BuildNonce(uint32 sequence, uint8 *out) const
    {
        MMO::BuildNonce(sequence, _clientRandom, out);
    }

} // namespace MMO
