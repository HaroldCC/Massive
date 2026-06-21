/**
 * @file CenterServer.cpp
 * @brief CenterServer 实现
 */

#include "Center/CenterServer.h"
#include "Center/CenterConfig.h"

#include "Common/Log/Log.h"
#include "Common/Network/TCPSocket.h"

namespace MMO
{

bool CenterServer::Init(const CenterConfig& cfg)
{
    _ioPool   = std::make_unique<IOContextPool>(static_cast<size_t>(cfg.network.ioThreads));
    _acceptor = std::make_unique<TCPAcceptor>(*_ioPool, cfg.network.port);

    _acceptor->Start([this](std::shared_ptr<TCPSocket> socket)
    {
        OnNewConnection(std::move(socket));
    });

    Log::Info("CenterServer listening on port {}", cfg.network.port);
    return true;
}

void CenterServer::Run()
{
    _running = true;
    _ioPool->Start();
}

void CenterServer::Stop()
{
    _running = false;
    if (_acceptor)
    {
        _acceptor->Stop();
    }
    if (_ioPool)
    {
        _ioPool->Stop();
    }
}

void CenterServer::OnNewConnection(std::shared_ptr<TCPSocket> socket)
{
    // TODO: 为连接注册 RPC handler（REGISTER / HEARTBEAT / QUERY_PLAYER_LOCATION ...）
    //       内部 RPC proto（CenterRPC.proto + EInternalMsgID）及 MessageDispatcher<RPCContext> 尚未建立。
    //       当前先留骨架，后续建设 CenterRPC proto + 内部 MsgID 后完成注册。
    (void)socket;
}

} // namespace MMO
