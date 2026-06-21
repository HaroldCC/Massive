/**
 * @file CenterServer.h
 * @brief CenterServer 主类——纯 IO 进程，服务协调中心
 *
 * 接受 World/Social 的内部 TCP 连接，RPC handler 操作 ServiceRegistry + PlayerLocationIndex。
 * 无 LogicThread，回调在 IO 线程直跑。
 */
#pragma once

#include <memory>

#include "Common/Core/Types.h"
#include "Common/Network/IOContextPool.h"
#include "Common/Network/TCPAcceptor.h"

#include "Center/PlayerLocationIndex.h"
#include "Center/ServiceRegistry.h"

namespace MMO
{

struct CenterConfig;

class CenterServer
{
public:
    bool Init(const CenterConfig& cfg);
    void Run();
    void Stop();

private:
    void OnNewConnection(std::shared_ptr<TCPSocket> socket);

    std::unique_ptr<IOContextPool> _ioPool;
    std::unique_ptr<TCPAcceptor>   _acceptor;
    ServiceRegistry                _services;
    PlayerLocationIndex            _playerIndex;
    bool                           _running = false;
};

} // namespace MMO
