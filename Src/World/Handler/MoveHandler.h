/**
 * @file MoveHandler.h
 * @brief MSG_MOVE_REQ 处理——位置更新 + AOI 广播
 *
 * 首条 post-login 游戏内协议，验证完整加密→解密→处理→加密→响应管线。
 */
#pragma once

#include "Common/Core/Types.h"
#include "Common/Core/ByteBuffer.h"
#include "World/WorldSession.h"

namespace MMO
{

    class MoveHandler
    {
    public:
        /**
         * @brief 处理移动请求
         * @param sessionID  Gate sessionId
         * @param ws         WorldSession 引用（LogicThread 独占）
         * @param body       protobuf 序列化数据（已解密）
         * @param len        数据长度
         * @param gateSendFn 出站回调
         */
        static void Handle(
            uint32                                  sessionID,
            WorldSession                           &ws,
            const uint8                            *body,
            size_t                                  len,
            std::function<void(uint32, ByteBuffer)> gateSendFn);
    };

} // namespace MMO
