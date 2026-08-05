#pragma once

#include <format>
#include <functional>

#include "Common/Core/Types.h"

namespace MMO::ECS
{
    enum class EEntityType : uint8
    {
        Player  = 1,
        NPC     = 2,
        Monster = 3,
    };

    /**
     * @brief 强类型实体 ID（uint64 位布局：scene(16) | index(28) | version(20)）
     *
     * 为什么用 struct 而非裸 uint64 / enum class：
     *   1. 与普通整数强区分（杜绝把 entityID 当 sessionID/accountID 用）——
     *      类比 EnTT 的 enum class entity。
     *   2. struct 比 enum class 灵活：可内置位运算封装（MakeEntityID/SceneOf/...），
     *      调用方无需到处 cast；enum class 做位运算必须 static_cast。
     *   3. 隐式 uint64 转换 + std::hash 特化：跨语言/跨协议边界（protobuf/脚本）
     *      零摩擦传递，同时保留强类型语义。
     *
     * 位布局（ECS_00 §4.1 契约，不可改动）：
     *   63          48 47                      20 19                  0
     *  ┌──────────────┬──────────────────────────┬────────────────────┐
     *  │  scene (16)  │        index (28)         │    version (20)    │
     *  └──────────────┴──────────────────────────┴────────────────────┘
     */
    struct EntityID
    {
        /** @brief 裸值（跨语言/序列化/比较用） */
        uint64 raw = 0;

        // ── 构造函数 ──

        constexpr EntityID() = default;

        constexpr EntityID(uint64 value) : raw(value)
        {
        } // 允许从整数构造（显式语义：值即 ID）

        // ── 比较 ──

        constexpr bool operator==(const EntityID &other) const
        {
            return raw == other.raw;
        }

        constexpr bool operator!=(const EntityID &other) const
        {
            return raw != other.raw;
        }

        // ── 布尔语义（用于 if (eid) / if (eid == Invalid)）──

        constexpr explicit operator bool() const
        {
            return raw != 0;
        }

        // ── 隐式转 uint64（跨语言/协议边界）──

        constexpr operator uint64() const
        {
            return raw;
        }

        // ── 哨兵 ──

        /** @brief 无效实体 ID */
        static constexpr EntityID Invalid()
        {
            return EntityID(0);
        }
    };

    // ── 位布局常量（ECS_00 §4.1 权威契约）──

    inline constexpr uint64 kSceneBits   = 16;
    inline constexpr uint64 kIndexBits   = 28;
    inline constexpr uint64 kVersionBits = 20;

    inline constexpr uint64 kSceneShift  = kIndexBits + kVersionBits;       // 48
    inline constexpr uint64 kIndexShift  = kVersionBits;                    // 20
    inline constexpr uint64 kVersionMask = (uint64(1) << kVersionBits) - 1; // 0xFFFFF
    inline constexpr uint64 kIndexMask   = (uint64(1) << kIndexBits) - 1;   // 0xFFFFFFF
    inline constexpr uint64 kSceneMask   = (uint64(1) << kSceneBits) - 1;   // 0xFFFF

    /** @brief 无效实体哨兵（兼容旧代码的 kInvalidEntityID 常量名） */
    inline constexpr EntityID kInvalidEntityID = EntityID::Invalid();

    /**
     * @brief 实体内部索引（EnTT registry 内下标，32 位）
     *
     * 与 EntityID 的关系：
     *   - EntityIndex == EntityID 的 index 字段（低 28 位），二者对齐
     *   - 但 EntityIndex 缺 scene/version，不是对外身份
     *
     * 用途：Grid 格子 / DirtyIndex 脏标记 / SystemAOI 候选集——每帧热路径，
     * 只存 32 位 index 省内存 + 少位运算；且这些容器本就是 per-scene 的。
     *
     * 为什么单独一个类型：与 EntityID（对外身份）和普通 uint32（sessionID 等）
     * 在类型层面区分，杜绝混用。
     */
    struct EntityIndex
    {
        uint32 raw = 0;

        constexpr EntityIndex() = default;

        constexpr EntityIndex(uint32 value) : raw(value)
        {
        }

        constexpr bool operator==(const EntityIndex &other) const
        {
            return raw == other.raw;
        }

        constexpr bool operator!=(const EntityIndex &other) const
        {
            return raw != other.raw;
        }

        constexpr explicit operator bool() const
        {
            return raw != 0;
        }

        constexpr operator uint32() const
        {
            return raw;
        }

        /** @brief 无效索引哨兵 */
        static constexpr EntityIndex Invalid()
        {
            return EntityIndex(0xFFFFFFFFu);
        }
    };

    // ── 位操作访问器 ──

    /** @brief 组装 EntityID */
    inline constexpr EntityID MakeEntityID(uint16 sceneID, uint32 index, uint32 version)
    {
        return EntityID((static_cast<uint64>(sceneID) << kSceneShift)
                        | (static_cast<uint64>(index & kIndexMask) << kIndexShift)
                        | (static_cast<uint64>(version) & kVersionMask));
    }

    /** @brief 取 scene 段 */
    inline constexpr uint16 SceneOf(EntityID eid)
    {
        return static_cast<uint16>((eid.raw >> kSceneShift) & kSceneMask);
    }

    /** @brief 取 index 段 */
    inline constexpr uint32 IndexOf(EntityID eid)
    {
        return static_cast<uint32>((eid.raw >> kIndexShift) & kIndexMask);
    }

    /** @brief 取 version 段 */
    inline constexpr uint32 VersionOf(EntityID eid)
    {
        return static_cast<uint32>(eid.raw & kVersionMask);
    }

    /** @brief 是否有效（raw != 0） */
    inline constexpr bool IsValidEntity(EntityID eid)
    {
        return eid.raw != 0;
    }

} // namespace MMO::ECS

// ── std::hash 特化（unordered_map/unordered_set 用）──

template <>
struct std::hash<MMO::ECS::EntityID>
{
    size_t operator()(const MMO::ECS::EntityID &eid) const noexcept
    {
        // 强类型包装直接哈希底层值——与哈希 uint64 等价，无额外开销
        return std::hash<uint64>()(eid.raw);
    }
};

template <>
struct std::hash<MMO::ECS::EntityIndex>
{
    size_t operator()(const MMO::ECS::EntityIndex &idx) const noexcept
    {
        return std::hash<uint32>()(idx.raw);
    }
};

// ── std::formatter 特化（Log::Info("{}", eid) 直接可用）──
// 格式：打印裸值（十进制）。诊断时可看 SceneOf/IndexOf/VersionOf 拆分。

template <>
struct std::formatter<MMO::ECS::EntityID> : std::formatter<uint64>
{
    auto format(const MMO::ECS::EntityID &eid, std::format_context &ctx) const
    {
        return std::formatter<uint64>::format(eid.raw, ctx);
    }
};

template <>
struct std::formatter<MMO::ECS::EntityIndex> : std::formatter<uint32>
{
    auto format(const MMO::ECS::EntityIndex &idx, std::format_context &ctx) const
    {
        return std::formatter<uint32>::format(idx.raw, ctx);
    }
};