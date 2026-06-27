/**
 * @file CenterClient.h
 * @brief World→Center 长连接客户端
 *
 * 职责：
 *   1. 连接 CenterServer
 *   2. 发送 RegisterWorldReq → 收到 RegisterWorldRsp
 *   3. 周期性 Heartbeat（5s）
 *   4. 借助 RPCClient（LogicThread 模式）发起 Call
 *
 * 断线重连：退避重连 + 重新 REGISTER（预留接口，MVP 先假设连接稳定）
 */
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <asio/ip/tcp.hpp>

#include "Common/Core/Types.h"
#include "Common/Network/IOContextPool.h"
#include "Common/Network/RPCClient.h"
#include "Common/Network/TCPSocket.h"

namespace MMO
{

    class CenterClient
    {
    public:
        CenterClient();

        /**
         * @brief 连接到 CenterServer
         * @param host      CenterServer 地址
         * @param port      CenterServer 端口
         * @param worldCfg  World 注册信息
         * @param ioPool    IOContextPool
         */
        bool Connect(const std::string &host,
                     uint16             port,
                     uint16             worldServerID,
                     uint16             maxPlayers,
                     const std::string &address,
                     IOContextPool     *ioPool);

        // 发送心跳（LogicThread 每 Tick 调用）
        void SendHeartbeat(uint32 currentPlayers);

        // 获取 RPCClient 引用
        RPCClient &GetRPCClient()
        {
            return _rpcClient;
        }

        // 获取连接状态
        bool IsConnected() const
        {
            return _connected.load(std::memory_order_acquire);
        }

        // 获取 Center TCPSocket（用于 Notify）
        std::shared_ptr<TCPSocket> GetSocket() const { return _socket; }

    private:
        std::shared_ptr<TCPSocket> _socket;
        RPCClient                  _rpcClient;  // LogicThread 模式
        std::atomic<bool>          _connected {false};
        uint16                     _worldServerID = 0;
        uint16                     _maxPlayers    = 0;
        std::string                _address;
    };

} // namespace MMO
