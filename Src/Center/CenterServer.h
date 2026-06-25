/**
 * @file CenterServer.h
 * @brief CenterServer 主类——纯 IO 进程，服务协调中心
 *
 * 接受 World/Social 的内部 TCP 连接，RPC handler 操作 ServiceRegistry + PlayerLocationIndex。
 * 无 LogicThread，回调在 IO 线程直跑。
 */
#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "Common/Core/Types.h"
#include "Common/Network/IOContextPool.h"
#include "Common/Network/RPCServerDispatcher.h"
#include "Common/Network/TCPAcceptor.h"

#include "Center/PlayerLocationIndex.h"
#include "Center/ServiceRegistry.h"

// Protobuf 前向声明（避免在 .h 中 include .pb.h，由 .cpp 引入完整定义）
namespace MMO
{

// 外部协议（Login/Common）
// （目前在 .h 中无前向声明需求）

} // namespace MMO

namespace MMO::Internal
{

// 内部 RPC 协议前向声明
class RegisterWorldReq;
class HeartbeatReq;
class PickWorldReq;
class QueryPlayerLocationReq;
class PlayerOnlineNtf;
class PlayerOfflineNtf;

} // namespace MMO::Internal

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
    // TCPAcceptor 回调
    void OnNewConnection(std::shared_ptr<TCPSocket> socket);

    // ── RPC handler ──
    void OnRegisterWorld(RPCContext ctx, const Internal::RegisterWorldReq& req);
    void OnHeartbeat(RPCContext ctx, const Internal::HeartbeatReq& req);
    void OnPickWorld(RPCContext ctx, const Internal::PickWorldReq& req);
    void OnQueryPlayerLocation(RPCContext ctx, const Internal::QueryPlayerLocationReq& req);
    void OnPlayerOnline(RPCContext ctx, const Internal::PlayerOnlineNtf& req);
    void OnPlayerOffline(RPCContext ctx, const Internal::PlayerOfflineNtf& req);

    /** @brief 将 socket remote endpoint 映射到 serviceID */
    std::string ServiceIDForSocket(const TCPSocket& socket) const;

    std::unique_ptr<IOContextPool> _ioPool;
    std::unique_ptr<TCPAcceptor>   _acceptor;
    RPCServerDispatcher            _rpcHandlers;
    ServiceRegistry                _services;
    PlayerLocationIndex            _playerIndex;

    /** @brief socket 指针 → serviceID（用于断线感知） */
    std::unordered_map<const TCPSocket*, std::string> _socketToService;

    bool _running = false;
};

} // namespace MMO
