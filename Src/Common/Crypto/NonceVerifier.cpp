/**
 * @file NonceVerifier.cpp
 * @brief Bloom filter 防重放校验实现
 */

#include "Common/Crypto/NonceVerifier.h"

namespace MMO::Crypto
{

    /** @brief 构造，初始化空的 Bloom filter */
    NonceVerifier::NonceVerifier() : _filter()
    {
    }

    /**
     * @brief 检查 nonce 是否已使用
     * @param nonce  待检查的 nonce 值
     * @return 首次出现 → true；已使用 / Bloom 误判 → false
     */
    bool NonceVerifier::CheckAndInsert(uint64 nonce)
    {
        if (ProbablySeen(nonce))
        {
            return false;
        }

        Insert(nonce);
        _recent.push_back({std::chrono::steady_clock::now(), nonce});

        return true;
    }

    /**
     * @brief 清理超过 maxAge 的 nonce
     * @param maxAge  过期时限
     */
    void NonceVerifier::EvictOld(std::chrono::hours maxAge)
    {
        auto now = std::chrono::steady_clock::now();
        while (!_recent.empty() && (now - _recent.front().first) > maxAge)
        {
            _recent.pop_front();
        }

        if (_recent.size() < kFilterBits / 16)
        {
            _filter.reset();
            for (auto &[ts, nonce] : _recent)
            {
                Insert(nonce);
            }
        }
    }

    /** @brief Bloom filter 碰撞检测 */
    bool NonceVerifier::ProbablySeen(uint64 nonce) const
    {
        return _filter.test(Hash1(nonce) % kFilterBits) && _filter.test(Hash2(nonce) % kFilterBits)
               && _filter.test(Hash3(nonce) % kFilterBits);
    }

    /** @brief 设置 Bloom filter 位 */
    void NonceVerifier::Insert(uint64 nonce)
    {
        _filter.set(Hash1(nonce) % kFilterBits);
        _filter.set(Hash2(nonce) % kFilterBits);
        _filter.set(Hash3(nonce) % kFilterBits);
    }

    /** @brief 基于 splitmix64 的哈希函数 1 */
    uint32 NonceVerifier::Hash1(uint64 v) const
    {
        v = v + 0x9e3779b97f4a7c15ULL;
        v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
        return static_cast<uint32>(v ^ (v >> 27));
    }

    /** @brief 基于 splitmix64 的哈希函数 2 */
    uint32 NonceVerifier::Hash2(uint64 v) const
    {
        v = v + 0x3c6ef372fe94f82aULL;
        v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
        return static_cast<uint32>(v ^ (v >> 31));
    }

    /** @brief 基于 splitmix64 的哈希函数 3 */
    uint32 NonceVerifier::Hash3(uint64 v) const
    {
        v = (v ^ (v >> 33)) * 0xff51afd7ed558ccdULL;
        v = (v ^ (v >> 33)) * 0xc4ceb9fe1a85ec53ULL;
        return static_cast<uint32>(v ^ (v >> 33));
    }

} // namespace MMO::Crypto
