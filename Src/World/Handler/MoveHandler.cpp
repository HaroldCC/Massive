/**
 * @file MoveHandler.cpp
 * @brief MoveHandler 实现——服务器权威校验 + 位置广播
 */

#include "World/Handler/MoveHandler.h"
#include "Common/Log/Log.h"

#include <Move.pb.h>
#include <MsgID.pb.h>

#include <chrono>

namespace MMO
{

    void MoveHandler::Handle(
        uint32                                  sessionID,
        WorldSession                           &ws,
        const uint8                            *body,
        size_t                                  len,
        std::function<void(uint32, ByteBuffer)> gateSendFn)
    {
        Proto::MoveReq req;
        if (!req.ParseFromArray(body, static_cast<int>(len)))
        {
            Log::Warn("MoveHandler: protobuf parse failed ({} bytes) session={}", len, sessionID);
            return;
        }

        // 服务器权威校验（MVP 基本检查）
        if (req.speed() < 0.0f || req.speed() > 50.0f)
        {
            Log::Debug("MoveHandler: invalid speed={} session={}", req.speed(), sessionID);
            return;
        }

        // 位置更新（MVP: 直接接受，后续 NavMesh 可达性校验）
        auto &pos = req.position();

        Log::Debug("MoveHandler: session={} pos=({:.1f}, {:.1f}, {:.1f}) spd={}",
                   sessionID, pos.x(), pos.y(), pos.z(), req.speed());

        // 响应：服务器位置（MVP 直接回传，不做纠正）
        Proto::MoveRsp rsp;
        rsp.set_sequence(req.sequence());
        rsp.mutable_position()->set_x(pos.x());
        rsp.mutable_position()->set_y(pos.y());
        rsp.mutable_position()->set_z(pos.z());
        rsp.set_server_time(static_cast<uint32>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));

        auto data = rsp.SerializeAsString();
        auto buf = ByteBuffer::Copy(
            reinterpret_cast<const uint8 *>(data.data()), data.size());
        gateSendFn(sessionID, std::move(buf));
    }

} // namespace MMO
