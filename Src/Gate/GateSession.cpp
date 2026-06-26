/**
 * @file GateSession.cpp
 * @brief GateSession 实现
 */
#include "Gate/GateSession.h"
#include "Common/Log/Log.h"

namespace MMO
{

    GateSession::GateSession(uint32 sessionID, std::shared_ptr<TCPSocket> socket)
        : _sessionID(sessionID)
        , _socket(std::move(socket))
        , _lastActive(std::chrono::steady_clock::now())
    {
    }

    GateSession::~GateSession()
    {
        Close();
    }

    /**
     * @brief 启动：TCPSocket 开始读循环
     */
    void GateSession::Start()
    {
        _socket->Start();
    }

    /**
     * @brief 关闭：幂等地关闭底层 TCPSocket
     */
    void GateSession::Close()
    {
        if (_socket)
        {
            _socket->Close();
        }
    }

    /**
     * @brief 向客户端发送数据
     * @param data  ByteBuffer（含 PacketHeader，Own 模式）
     */
    void GateSession::SendToClient(ByteBuffer data)
    {
        if (_socket)
        {
            _socket->Send(std::move(data));
        }
    }

    /**
     * @brief 更新最后活跃时间戳为当前时刻
     */
    void GateSession::UpdateActiveTime()
    {
        _lastActive = std::chrono::steady_clock::now();
    }

    /**
     * @brief 计算距上次活跃的秒数
     * @return idle 秒数
     */
    uint64 GateSession::IdleSeconds() const
    {
        auto now = std::chrono::steady_clock::now();
        return static_cast<uint64>(
            std::chrono::duration_cast<std::chrono::seconds>(now - _lastActive).count());
    }

} // namespace MMO
