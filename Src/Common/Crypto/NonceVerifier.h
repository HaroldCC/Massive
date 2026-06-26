/**
 * @file NonceVerifier.h
 * @brief Bloom filter 防重放校验
 *
 * 128KB Bloom filter + 3 hash funcs + 8 小时过期队列。
 * ~1% 误判率——误拒通过客户端自动重试消解。
 */
#pragma once

#include <bitset>
#include <chrono>
#include <deque>

#include "Common/Core/Types.h"

namespace MMO::Crypto
{

    /**
     * @brief Bloom filter 防重放校验器
     *
     * 服务端每收到一个 nonce，先查 Bloom filter：
     * - 未见 → 记录并允许
     * - 已见 → 拒绝（防重放）
     * 定期清理过期 nonce 并重建 filter 以控制误判率。
     */
    class NonceVerifier
    {
    public:
        NonceVerifier();

        /**
         * @brief 检查 nonce 是否已使用
         * @param nonce  待检查的 nonce 值
         * @return 首次出现 → true；已使用 / Bloom 误判 → false
         */
        bool CheckAndInsert(uint64 nonce);

        /**
         * @brief 清理过期 nonce（每 tick 调用一次）
         * @param maxAge  过期时限
         */
        void EvictOld(std::chrono::hours maxAge);

    private:
        /**
 * @brief Bloom filter 碰撞检测
 */
        bool ProbablySeen(uint64 nonce) const;
        /**
 * @brief 插入 Bloom filter
 */
        void Insert(uint64 nonce);

        /**
 * @brief 基于 splitmix64 的 3 个哈希函数
 */
        uint32 Hash1(uint64 v) const;
        uint32 Hash2(uint64 v) const;
        uint32 Hash3(uint64 v) const;

        static constexpr size_t kFilterBits = 1 << 20; // 1M bits = 128KB
        static constexpr size_t kHashFuncs  = 3;       // 哈希函数数量

        std::bitset<kFilterBits>                                             _filter;
        std::deque<std::pair<std::chrono::steady_clock::time_point, uint64>> _recent;
    };

} // namespace MMO::Crypto
