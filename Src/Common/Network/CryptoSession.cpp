/**
 * @file CryptoSession.cpp
 * @brief per-session AES-256-GCM 加解密上下文实现
 */

#include "Common/Network/CryptoSession.h"
#include "Common/Network/PacketHeader.h"
#include "Common/Crypto/Aes256Gcm.h"

#include <cstring>

namespace MMO
{

void CryptoSession::Init(const uint8* sessionKey, uint64 clientRandom)
{
    std::memcpy(_sessionKey, sessionKey, 32);
    _clientRandom = clientRandom;
    // 序列号从 0 开始，首次 Encrypt 返回 1
    _lastSequence = 0;
    _sendSequence = 0;
}

ByteBuffer CryptoSession::Encrypt(const uint8* plaintext, size_t len)
{
    uint32 seq = ++_sendSequence;

    uint8 nonce[12];
    BuildNonce(seq, nonce);

    auto result = Crypto::Aes256Gcm::Encrypt(_sessionKey, nonce, plaintext, len);
    if (!result)
    {
        return ByteBuffer{};
    }

    return std::move(*result);
}

std::optional<ByteBuffer> CryptoSession::Decrypt(
    const uint8* ciphertext,
    size_t len,
    uint32 seq)
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
    uint32 diff = seq - _lastSequence;
    return diff > 0 && diff < kHalfRange;
}

void CryptoSession::BuildNonce(uint32 sequence, uint8* out) const
{
    MMO::BuildNonce(sequence, _clientRandom, out);
}

} // namespace MMO
