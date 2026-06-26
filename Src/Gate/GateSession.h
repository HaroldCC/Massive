/**
 * @file GateSession.h
 * @brief Gate 侧会话——包装 TCPSocket + sessionId + 路由状态
 *
 * GateSession 不自建 socket，包装已有 TCPSocket。
 * 粘包拆包/异步写链/生命周期全部由 TCPSocket 提供。
 */
#pragma once

#include <memory>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"
#include "Common/Network/TCPSocket.h"

namespace MMO
{

    /**
     * @brief Gate 侧会话
     *
     * 极简，无业务逻辑。生命周期 = TCP 连接建立 → 断开。
     * _routed = true 后表示已完成 EnterWorld 路由，消息将透传到 World。
     */
    class GateSession : public std::enable_shared_from_this<GateSession>
    {
    public:
        /**
         * @brief 构造
         * @param sessionID  Gate 分配的会话 ID
         * @param socket     客户端 TCPSocket（已 accept，未 Start）
         */
        explicit GateSession(uint32 sessionID, std::shared_ptr<TCPSocket> socket);

        GateSession(const GateSession &)            = delete;
        GateSession &operator=(const GateSession &) = delete;

        ~GateSession();

        /**
         * @brief 启动：注册 TCPSocket 回调 → Start
         */
        void Start();

        /**
         * @brief 关闭（主动关闭客户端连接）
         */
        void Close();

        /**
         * @brief 向客户端发送数据
         * @param data  ByteBuffer（含 PacketHeader，Own 模式）
         */
        void SendToClient(ByteBuffer data);

        // ── 访问器 ──

        uint32 SessionID() const
        {
            return _sessionID;
        }

        uint16 WorldServerID() const
        {
            return _worldServerID;
        }

        void SetWorldServerID(uint16 id)
        {
            _worldServerID = id;
        }

        bool IsRouted() const
        {
            return _routed;
        }

        void SetRouted()
        {
            _routed = true;
        }

        void SetRouted(bool routed)
        {
            _routed = routed;
        }

        std::shared_ptr<TCPSocket> &Socket()
        {
            return _socket;
        }

        /**
         * @brief 更新最后活跃时间（心跳/消息触发）
         */
        void UpdateActiveTime();

        /**
         * @brief 距上次活跃的秒数
         */
        uint64 IdleSeconds() const;

        std::chrono::steady_clock::time_point GetLastActive() const
        {
            return _lastActive;
        }

    private:
        uint32                     _sessionID;             // Gate 分配的会话标识
        uint16                     _worldServerID = 0;     // 从 SessionToken[0..1] 解析的目标 World
        bool                       _routed        = false; // 已完成 EnterWorld 路由
        std::shared_ptr<TCPSocket> _socket;                // 客户端 TCP 连接

        std::chrono::steady_clock::time_point _lastActive; // 最后心跳/消息时间戳
    };

} // namespace MMO
