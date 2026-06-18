/**
 * @file SessionToken.cpp
 * @brief SessionToken 签发与验证
 */

#include "Common/Crypto/SessionToken.h"
#include "Common/Crypto/Aes256Ecb.h"
#include "Common/Crypto/HmacSha256.h"

#include <cstring>
#include <ctime>

namespace MMO::Crypto
{

/**
 * @brief 签发 SessionToken
 * @param lss             LoginServerSecret (32B, 与 WorldServer 共享)
 * @param sessionKey      ECDH 产生的 32B SessionKey
 * @param worldServerId   目标 WorldServer ID
 * @param accountId       玩家账号 ID
 * @param expireTime      过期时间戳
 * @return SessionToken，失败返回 nullopt
 */
std::optional<SessionToken> SessionTokenBuilder::Issue(
    const uint8* lss,
    const uint8* sessionKey,
    uint16 worldServerId,
    uint32 accountId,
    uint32 expireTime)
{
    SessionToken token;

    token.SetWorldServerId(worldServerId);
    token.SetAccountId(accountId);
    token.SetExpireTime(expireTime);

    auto encrypted = Aes256Ecb::Encrypt(lss, sessionKey, Aes256Ecb::kKeySize);
    if (!encrypted || encrypted->Size() != SessionToken::kKeyEncSize)
    {
        return std::nullopt;
    }
    std::memcpy(token.EncryptedSessionKey(), encrypted->Data(), SessionToken::kKeyEncSize);

    auto hmac = HmacSha256::Sign(lss, Aes256Ecb::kKeySize, token.data,
        SessionToken::kPlainSize + SessionToken::kKeyEncSize);
    if (!hmac || hmac->Size() < SessionToken::kHmacSize)
    {
        return std::nullopt;
    }
    std::memcpy(token.Hmac(), hmac->Data(), SessionToken::kHmacSize);

    return token;
}

/**
 * @brief WorldServer 验证 SessionToken
 * @param lss    LoginServerSecret (32B)
 * @param token  待验证的 SessionToken
 * @return TokenPayload，验证失败返回 nullopt
 */
std::optional<SessionTokenBuilder::TokenPayload> SessionTokenBuilder::Verify(
    const uint8* lss,
    const SessionToken& token)
{
    auto expectedHmac = HmacSha256::Sign(lss, Aes256Ecb::kKeySize, token.data,
        SessionToken::kPlainSize + SessionToken::kKeyEncSize);
    if (!expectedHmac || expectedHmac->Size() < SessionToken::kHmacSize)
    {
        return std::nullopt;
    }

    if (std::memcmp(expectedHmac->Data(), token.Hmac(), SessionToken::kHmacSize) != 0)
    {
        return std::nullopt;
    }

    auto decrypted = Aes256Ecb::Decrypt(lss, token.EncryptedSessionKey(), SessionToken::kKeyEncSize);
    if (!decrypted || decrypted->Size() != Aes256Ecb::kKeySize)
    {
        return std::nullopt;
    }

    TokenPayload payload;
    payload.accountId = token.AccountId();
    payload.expireTime = token.ExpireTime();
    payload.sessionKey = std::move(*decrypted);

    if (payload.expireTime < static_cast<uint32>(std::time(nullptr)))
    {
        return std::nullopt;
    }

    return payload;
}

} // namespace MMO::Crypto
