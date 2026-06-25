/**
 * @file PacketHeader.h
 * @brief 网络协议 Wire 格式定义
 *
 * 客户端 ↔ GateServer 的线格式（大端序）：
 *   [PacketHeader | Body]
 *
 * GateServer ↔ WorldServer 的内部格式：
 *   [InternalHeader | MsgID(4B) | EncryptedBody]
 */
#pragma once

#include <cstdint>
#include <cstring>

#include "Common/Core/Types.h"

namespace MMO
{

/**
 * @brief 客户端 ↔ Gate 线格式头（12 字节）
 *
 * 所有包固定以 PacketHeader 开头，大端序。
 * GateServer 只读 sessionId 做路由，不解密 Body。
 */
#pragma pack(push, 1)

    struct PacketHeader
    {
        uint32 length;    // 整包长度（含头，大端）
        uint32 msgID;     // EMsgID 枚举值（大端）
        uint32 sessionID; // Gate 分配的会话 ID（明文，Gate 路由用）
    };

#pragma pack(pop)

/**
 * @brief GateServer ↔ WorldServer 内部帧（大端序）
 *
 * InternalHeader + MsgID + EncryptedBody。
 */
#pragma pack(push, 1)

    struct InternalHeader
    {
        uint32 sessionID; // 目标/来源 sessionId（大端）
    };

#pragma pack(pop)

    /**
     * @brief 从长整型构造 nonce（12 字节 AES-GCM IV）
     * @param sequence     序列号
     * @param clientRandom 客户端随机前缀
     * @param out          输出缓冲区（必须 >= 12 字节）
     */
    inline void BuildNonce(uint32 sequence, uint64 clientRandom, uint8 *out)
    {
        // sequence: 大端 4 字节
        out[0] = static_cast<uint8>(sequence >> 24);
        out[1] = static_cast<uint8>(sequence >> 16);
        out[2] = static_cast<uint8>(sequence >> 8);
        out[3] = static_cast<uint8>(sequence);

        // clientRandom: 大端 8 字节
        out[4]  = static_cast<uint8>(clientRandom >> 56);
        out[5]  = static_cast<uint8>(clientRandom >> 48);
        out[6]  = static_cast<uint8>(clientRandom >> 40);
        out[7]  = static_cast<uint8>(clientRandom >> 32);
        out[8]  = static_cast<uint8>(clientRandom >> 24);
        out[9]  = static_cast<uint8>(clientRandom >> 16);
        out[10] = static_cast<uint8>(clientRandom >> 8);
        out[11] = static_cast<uint8>(clientRandom);
    }

} // namespace MMO
