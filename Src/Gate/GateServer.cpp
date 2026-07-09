/**
 * @file GateServer.cpp
 * @brief GateServer 实现——无状态连接代理
 *
 * 线程模型：纯 IOContextPool 多线程，无 LogicThread。
 * 核心消息路由在 IO 线程中短回调执行，主线程只做超时检查。
 */
#include "Gate/GateServer.h"
#include "Gate/GateConfig.h"
#include "Gate/GateSession.h"

#include "Common/Core/MassiveAssert.h"
#include "Common/Core/ByteBuffer.h"
#include "Common/Log/Log.h"
#include "Common/Network/PacketHeader.h"

// Proto 完整定义——需要反序列化消息
#include <Login.pb.h>
#include <MsgID.pb.h>

#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>

#include <cstring>
#include <thread>

namespace MMO
{

    // ── 内部常量 ──

    /**
     * @brief InternalHeader 大小 = sessionID(4B)
     */
    static constexpr size_t kInternalHeaderSize = sizeof(uint32);

    // ── 生命周期 ──

    /**
     * @brief 初始化 GateServer
     * @param cfg  GateConfig
     * @return 成功返回 true
     */
    bool GateServer::Init(const GateConfig &cfg)
    {
        _config = cfg;

        _ioPool = std::make_unique<IOContextPool>(static_cast<size_t>(cfg.network.ioThreads));

        // 监听客户端连接（PacketHeader 帧协议）
        _acceptor = std::make_unique<TCPAcceptor>(*_ioPool, cfg.network.port, EFraming::PacketHeader);

        _acceptor->Start([this](std::shared_ptr<TCPSocket> socket) {
            OnNewClientConnection(std::move(socket));
        });

        // 连接到所有 WorldServer
        ConnectToWorlds(cfg);

        Log::Info("GateServer listening on port {} ({} io threads, max {} conns)",
                  cfg.network.port,
                  cfg.network.ioThreads,
                  cfg.network.maxConnections);
        return true;
    }

    /**
     * @brief 启动 IO 线程池 + 超时检查
     */
    void GateServer::Run()
    {
        _running.store(true, std::memory_order_release);

        // 启动 IO 线程池
        _ioPool->Start();

        // 启动超时检查定时器（复用第一个 io_context）
        StartTimeoutCheck();

        // 主线程等待
        while (_running.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    /**
     * @brief 停止所有组件
     */
    void GateServer::Stop()
    {
        _running.store(false, std::memory_order_release);
        if (_acceptor)
        {
            _acceptor->Stop();
        }
        if (_ioPool)
        {
            _ioPool->Stop();
        }
    }

    // ── 新客户端连接 ──

    /**
     * @brief 新客户端连接
     * @param socket  已 accept 的 TCPSocket
     */
    void GateServer::OnNewClientConnection(std::shared_ptr<TCPSocket> socket)
    {
        // 获取客户端 IP
        std::string clientIP = "0.0.0.0";
        try
        {
            clientIP = socket->LowestLayer().remote_endpoint().address().to_string();
        }
        catch (const std::exception &e)
        {
            Log::Error("GateServer: remote_endpoint failed: {}", e.what());
        }
        catch (...)
        {
            Log::Error("GateServer: remote_endpoint failed (unknown)");
        }

        // 限流检查
        if (!AllowNewConnection(clientIP))
        {
            Log::Debug("GateServer: connection rejected from {} (rate limit)", clientIP);
            socket->Close();
            return;
        }

        // 分配 sessionId 并创建 GateSession
        uint32 sessionID = AllocateSessionID();
        auto   session   = std::make_shared<GateSession>(sessionID, std::move(socket));

        // 注册消息回调
        auto weakSession = std::weak_ptr<GateSession>(session);

        session->Socket()->SetMessageHandler(
            [this, weakSession](uint32 msgID, uint32 /*packetSessionID*/, const uint8 *body, size_t len) {
                auto s = weakSession.lock();
                if (s)
                {
                    OnClientMessage(std::move(s), msgID, body, len);
                }
            });

        // 注册断线回调
        session->Socket()->SetCloseHandler([this, sessionID, clientIP]() {
            OnSessionDisconnect(sessionID);
            OnConnectionClosed(clientIP);
        });

        // 记录 session
        {
            std::lock_guard lock(_gateMutex);
            _sessions[sessionID] = session;
        }

        Log::Info("GateServer: new client sessionId={} from {}", sessionID, clientIP);

        session->Start();
    }

    // ── 客户端消息处理 ──

    /**
     * @brief 客户端消息入口
     * @param session  来源 GateSession
     * @param msgID    消息 ID
     * @param body     消息体
     * @param len      消息体长度
     */
    void GateServer::OnClientMessage(std::shared_ptr<GateSession> session,
                                     uint32                       msgID,
                                     const uint8                 *body,
                                     size_t                       len)
    {
        session->UpdateActiveTime();

        // ── 心跳：Gate 自回 ──
        if (msgID == Proto::MSG_HEARTBEAT_REQ)
        {
            Proto::HeartbeatReq req;
            if (req.ParseFromArray(body, static_cast<int>(len)))
            {
                OnHeartbeatReq(std::move(session), req);
            }
            return;
        }

        // ── 未路由状态：只接受 EnterWorld ──
        if (!session->IsRouted())
        {
            if (msgID == Proto::MSG_LOGIN_ENTER_WORLD_REQ)
            {
                Proto::LoginEnterWorldReq req;
                if (req.ParseFromArray(body, static_cast<int>(len)))
                {
                    OnEnterWorldReq(std::move(session), req, body, len);
                }
            }
            else
            {
                Log::Debug("GateServer: dropping msgID={} from un-routed session {}",
                           msgID,
                           session->SessionID());
            }
            return;
        }

        // ── 已路由：透传到 WorldServer ──
        ForwardToWorld(session, msgID, body, len);
    }

    // ── 进入世界路由 ──

    /**
     * @brief 进入世界请求处理
     * @param session  客户端会话
     * @param req      反序列化后的请求
     * @param rawBody  原始 body 指针（转发用）
     * @param rawLen   原始 body 长度
     */
    void GateServer::OnEnterWorldReq(std::shared_ptr<GateSession>     session,
                                     const Proto::LoginEnterWorldReq &req,
                                     const uint8                     *rawBody,
                                     size_t                           rawLen)
    {
        const auto &tokenBytes = req.session_token();
        if (static_cast<size_t>(tokenBytes.size()) < 2)
        {
            Log::Debug("GateServer: EnterWorld with invalid session token (size={})", tokenBytes.size());
            SendEnterWorldError(session, 2001);
            return;
        }

        // 从 SessionToken[0..1] 读取 worldServerId（大端）
        const auto *tokenRaw = reinterpret_cast<const uint8 *>(tokenBytes.data());
        uint16      worldId =
            static_cast<uint16>((static_cast<uint16>(tokenRaw[0]) << 8) | static_cast<uint16>(tokenRaw[1]));

        // 找对应 WorldServer 地址（MVP：第一个可用的）
        std::string worldAddr = PickWorldServer();
        if (worldAddr.empty())
        {
            Log::Warn("GateServer: no world available for sessionId={}", session->SessionID());
            SendEnterWorldError(session, 2002);
            return;
        }

        // 标记已路由
        session->SetWorldServerID(worldId);
        session->SetRouted();

        // 记录路由表
        {
            std::lock_guard lock(_gateMutex);
            _sessionRoutes[session->SessionID()] =
                SessionRoute {worldAddr, std::weak_ptr<GateSession>(session)};
        }

        // 记录 world→sessions 映射
        {
            std::lock_guard lock(_gateMutex);
            _worldSessionMap[worldAddr].push_back(session->SessionID());
        }

        Log::Info("GateServer: routed sessionId={} to world '{}' (worldId={})",
                  session->SessionID(),
                  worldAddr,
                  worldId);

        // 转发 EnterWorldReq 到 World
        ForwardToWorld(session, Proto::MSG_LOGIN_ENTER_WORLD_REQ, rawBody, rawLen);
    }

    /**
     * @brief 进入世界失败回应，关闭连接
     * @param session    客户端会话
     * @param errorCode  错误码
     */
    void GateServer::SendEnterWorldError(std::shared_ptr<GateSession> session, uint32 errorCode)
    {
        Proto::LoginEnterWorldRsp rsp;
        rsp.mutable_error()->set_code(errorCode);

        switch (errorCode)
        {
            case 2001:
                rsp.mutable_error()->set_message("Invalid session token");
                break;
            case 2002:
                rsp.mutable_error()->set_message("No world server available");
                break;
            default:
                rsp.mutable_error()->set_message("Enter world failed");
                break;
        }

        // 零分配序列化
        size_t bodySize = static_cast<size_t>(rsp.ByteSizeLong());
        auto   bodyBuf  = ByteBuffer::Own(bodySize);
        rsp.SerializeToArray(bodyBuf.WritePtr(), static_cast<int>(bodySize));
        bodyBuf.SetWritePos(bodySize);

        // 构建 PacketHeader + Body
        uint32 totalLen = static_cast<uint32>(sizeof(PacketHeader) + bodySize);
        auto   buf      = ByteBuffer::Own(totalLen);

        buf.WriteUint32(totalLen);
        buf.WriteUint32(Proto::MSG_LOGIN_ENTER_WORLD_RSP);
        buf.WriteUint32(session->SessionID());
        buf.WriteBytes(bodyBuf.Data(), bodyBuf.Size());

        session->SendToClient(std::move(buf));
        session->Close();
    }

    // ── 心跳（Gate 自回）──

    /**
     * @brief 心跳请求（Gate 自回）
     * @param session  客户端会话
     * @param req      心跳请求
     */
    void GateServer::OnHeartbeatReq(std::shared_ptr<GateSession>                session,
                                    [[maybe_unused]] const Proto::HeartbeatReq &req)
    {
        Proto::HeartbeatRsp rsp;
        auto nowMs = static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());
        rsp.set_server_time(nowMs);

        // 零分配序列化
        size_t bodySize = static_cast<size_t>(rsp.ByteSizeLong());
        auto   bodyBuf  = ByteBuffer::Own(bodySize);
        rsp.SerializeToArray(bodyBuf.WritePtr(), static_cast<int>(bodySize));
        bodyBuf.SetWritePos(bodySize);

        // 构建 PacketHeader + Body
        uint32 totalLen = static_cast<uint32>(sizeof(PacketHeader) + bodySize);
        auto   buf      = ByteBuffer::Own(totalLen);

        buf.WriteUint32(totalLen);
        buf.WriteUint32(Proto::MSG_HEARTBEAT_RSP);
        buf.WriteUint32(session->SessionID());
        buf.WriteBytes(bodyBuf.Data(), bodyBuf.Size());

        session->SendToClient(std::move(buf));
    }

    // ── 转发到 World ──

    /**
     * @brief 转发消息到 World，重建客户端包 + 包装 InternalHeader
     * @param session  客户端会话
     * @param msgID    消息 ID
     * @param body     消息体
     * @param bodyLen  消息体长度
     */
    void GateServer::ForwardToWorld(const std::shared_ptr<GateSession> &session,
                                    uint32                              msgID,
                                    const uint8                        *body,
                                    size_t                              bodyLen)
    {
        // 找目标 World 地址
        std::string worldAddr;
        {
            std::lock_guard lock(_gateMutex);
            auto            routeIt = _sessionRoutes.find(session->SessionID());
            if (routeIt == _sessionRoutes.end())
            {
                return; // 无路由
            }
            worldAddr = routeIt->second.worldAddr;
        }

        // 重建完整客户端包：[PacketHeader:12B][Body]
        uint32 packetHeaderSize = static_cast<uint32>(sizeof(PacketHeader));
        uint32 clientPacketSize = packetHeaderSize + static_cast<uint32>(bodyLen);

        ByteBuffer clientPacket = ByteBuffer::Own(clientPacketSize);
        clientPacket.WriteUint32(clientPacketSize);     // PacketHeader.length
        clientPacket.WriteUint32(msgID);                // PacketHeader.msgID
        clientPacket.WriteUint32(session->SessionID()); // PacketHeader.sessionID
        clientPacket.WriteBytes(body, bodyLen);         // Body

        // 构建 World 帧：[TotalLength:4B][InternalHeader:4B=sessionID][clientPacket]
        uint32 internalSize   = static_cast<uint32>(kInternalHeaderSize);
        uint32 payloadSize    = internalSize + clientPacketSize;
        uint32 totalFrameSize = static_cast<uint32>(sizeof(uint32)) + payloadSize;

        ByteBuffer frame = ByteBuffer::Own(totalFrameSize);
        frame.WriteUint32(totalFrameSize);                       // LengthPrefix
        frame.WriteUint32(session->SessionID());                 // InternalHeader.sessionID
        frame.WriteBytes(clientPacket.Data(), clientPacketSize); // 原始客户端包

        // 发送到 World
        {
            std::lock_guard lock(_gateMutex);
            auto            connIt = _worldConns.find(worldAddr);
            if (connIt == _worldConns.end() || !connIt->second->connected)
            {
                Log::Warn("GateServer: world '{}' not connected, dropping msgID={} sessionId={}",
                          worldAddr,
                          msgID,
                          session->SessionID());
                return;
            }
            connIt->second->socket->Send(std::move(frame));
        }
    }

    // ── World → Gate 出站 ──

    /**
     * @brief World → Gate 消息，剥离 InternalHeader 后转发客户端
     * @param worldAddr  WorldServer 地址
     * @param data       接收数据（不含 LengthPrefix）
     * @param len        数据长度
     */
    void GateServer::OnWorldMessage(const std::string &worldAddr, const uint8 *data, size_t len)
    {
        // data = [InternalHeader:4B][PacketHeader:12B][Body]
        // len = totalLen - 4 (LengthPrefix 已被 TCPSocket LengthPrefix 模式剥离)
        if (len < kInternalHeaderSize)
        {
            Log::Warn("GateServer: undersized world packet from {} ({} bytes)", worldAddr, len);
            return;
        }

        // 解析 InternalHeader（大端）
        uint32 sessionID = ByteBuffer::Wrap(data, 4).ReadUint32();

        // 剥离 InternalHeader，剩余：[PacketHeader:12B][Body]
        size_t       clientPacketLen = len - kInternalHeaderSize;
        const uint8 *clientPacket    = data + kInternalHeaderSize;

        // 查找目标客户端
        std::shared_ptr<GateSession> clientSession;
        {
            std::lock_guard lock(_gateMutex);
            auto            routeIt = _sessionRoutes.find(sessionID);
            if (routeIt == _sessionRoutes.end())
            {
                Log::Debug("GateServer: no route for sessionId={} (already disconnected)", sessionID);
                return;
            }
            clientSession = routeIt->second.clientSession.lock();
            if (!clientSession)
            {
                _sessionRoutes.erase(routeIt);
                return;
            }
        }

        // 写客户端（原始 [PacketHeader|Body]，不含 InternalHeader）
        auto buf = ByteBuffer::Copy(clientPacket, clientPacketLen);
        clientSession->SendToClient(std::move(buf));
    }

    // ── World 连接管理 ──

    /**
     * @brief 连接到配置中所有 WorldServer
     * @param cfg  GateConfig
     */
    void GateServer::ConnectToWorlds(const GateConfig &cfg)
    {
        for (const auto &addr : cfg.world.servers)
        {
            auto conn         = std::make_unique<WorldConnection>();
            _worldConns[addr] = std::move(conn);
            ConnectToWorld(addr);
        }
    }

    /**
     * @brief 连接到单个 WorldServer
     * @param addr  "ip:port" 格式地址
     */
    void GateServer::ConnectToWorld(const std::string &addr)
    {
        // 解析 "ip:port"
        auto colon = addr.find(':');
        if (colon == std::string::npos)
        {
            Log::Error("GateServer: invalid world address '{}'", addr);
            return;
        }

        std::string host = addr.substr(0, colon);
        uint16      port = static_cast<uint16>(std::stoi(addr.substr(colon + 1)));

        auto &ctx = _ioPool->GetNextContext();

        auto socket = std::make_shared<TCPSocket>(asio::ip::tcp::socket(ctx), EFraming::LengthPrefix);

        asio::ip::tcp::resolver resolver(ctx);
        asio::error_code        resolveEc;
        auto                    endpoints = resolver.resolve(host, std::to_string(port), resolveEc);
        if (resolveEc || endpoints.empty())
        {
            Log::Error("GateServer: resolve {} failed: {}", addr, resolveEc.message());
            ScheduleReconnect(addr);
            return;
        }

        socket->LowestLayer().async_connect(
            *endpoints.begin(),
            [this, addr, socket](const asio::error_code &ec) {
                if (ec)
                {
                    Log::Error("GateServer: connect to '{}' failed: {}", addr, ec.message());
                    ScheduleReconnect(addr);
                    return;
                }
                OnWorldConnected(addr, socket);
            });
    }

    /**
     * @brief World 连接建立成功回调
     * @param addr     WorldServer 地址
     * @param socket   已连接 TCPSocket
     */
    void GateServer::OnWorldConnected(const std::string &addr, std::shared_ptr<TCPSocket> socket)
    {
        {
            std::lock_guard lock(_gateMutex);
            auto            it = _worldConns.find(addr);
            if (it == _worldConns.end())
            {
                return;
            }
            it->second->socket           = socket;
            it->second->connected        = true;
            it->second->reconnectDelayMs = 1000; // 重置退避
        }

        // 注册 World 消息回调
        auto addrCopy = std::make_shared<std::string>(addr);

        socket->SetMessageHandler(
            [this, addrCopy](uint32 /*msgID*/, uint32 /*sessionID*/, const uint8 *body, size_t len) {
                OnWorldMessage(*addrCopy, body, len);
            });

        socket->SetCloseHandler([this, addr]() {
            OnWorldDisconnected(addr);
        });

        socket->Start();

        Log::Info("GateServer: connected to world '{}'", addr);
    }

    /**
     * @brief World 连接断开回调，清理路由 + 退避重连
     * @param addr  WorldServer 地址
     */
    void GateServer::OnWorldDisconnected(const std::string &addr)
    {
        Log::Warn("GateServer: world '{}' disconnected", addr);

        {
            std::lock_guard lock(_gateMutex);
            auto            it = _worldConns.find(addr);
            if (it != _worldConns.end())
            {
                it->second->connected = false;
                it->second->socket.reset();
            }
        }

        // 清理路由到该 World 的 session
        {
            std::lock_guard lock(_gateMutex);
            for (auto it = _sessionRoutes.begin(); it != _sessionRoutes.end();)
            {
                if (it->second.worldAddr == addr)
                {
                    it = _sessionRoutes.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        {
            std::lock_guard lock(_gateMutex);
            _worldSessionMap.erase(addr);
        }

        // 退避重连
        ScheduleReconnect(addr);
    }

    /**
     * @brief 退避重连（1s→2s→4s→...→30s max）
     * @param addr  "ip:port" 格式地址
     */
    void GateServer::ScheduleReconnect(const std::string &addr)
    {
        uint32 delayMs;
        {
            std::lock_guard lock(_gateMutex);
            auto            it = _worldConns.find(addr);
            if (it == _worldConns.end())
            {
                return;
            }
            delayMs                      = it->second->reconnectDelayMs;
            it->second->reconnectDelayMs = std::min(delayMs * 2, 30000u);
        }

        Log::Info("GateServer: reconnecting to '{}' in {}ms", addr, delayMs);

        // 用 asio steady_timer 做退避重连
        auto &ctx   = _ioPool->GetNextContext();
        auto  timer = std::make_shared<asio::steady_timer>(ctx, std::chrono::milliseconds(delayMs));

        timer->async_wait([this, addr, timer](const asio::error_code &ec) {
            if (ec)
            {
                return; // timer 被取消
            }
            ConnectToWorld(addr);
        });
    }

    // ── Session 管理 ──

    /**
     * @brief session 断开处理，清理路由和映射
     * @param sessionID  断开的 sessionId
     */
    void GateServer::OnSessionDisconnect(uint32 sessionID)
    {
        Log::Debug("GateServer: session {} disconnected", sessionID);

        // 清理路由表
        {
            std::lock_guard lock(_gateMutex);
            _sessionRoutes.erase(sessionID);
        }

        // 清理 world→sessions 映射
        {
            std::lock_guard lock(_gateMutex);
            for (auto &[addr, sessions] : _worldSessionMap)
            {
                auto it = std::find(sessions.begin(), sessions.end(), sessionID);
                if (it != sessions.end())
                {
                    sessions.erase(it);
                    break;
                }
            }
        }

        // 清理 session 本身
        {
            std::lock_guard lock(_gateMutex);
            _sessions.erase(sessionID);
        }
    }

    /**
     * @brief 清理 session（委托 OnSessionDisconnect）
     * @param sessionID  目标 sessionId
     */
    void GateServer::RemoveSession(uint32 sessionID)
    {
        OnSessionDisconnect(sessionID);
    }

    /**
     * @brief 根据 sessionId 找到目标 World 地址
     * @param sessionID  会话 ID
     * @return World 地址，未路由返回空字符串
     */
    std::string GateServer::GetWorldRoute(uint32 sessionID) const
    {
        std::lock_guard lock(_gateMutex);
        auto            it = _sessionRoutes.find(sessionID);
        if (it != _sessionRoutes.end())
        {
            return it->second.worldAddr;
        }
        return {};
    }

    /**
     * @brief 找到第一个可用的 World
     * @return World 地址，无可用返回空字符串
     */
    std::string GateServer::PickWorldServer() const
    {
        std::lock_guard lock(_gateMutex);
        for (const auto &[addr, conn] : _worldConns)
        {
            if (conn->connected)
            {
                return addr;
            }
        }
        return {};
    }

    // ── 限流 ──

    /**
     * @brief 是否允许新连接
     * @param clientIP  客户端 IP
     * @return 允许返回 true
     */
    bool GateServer::AllowNewConnection(const std::string &clientIP)
    {
        std::lock_guard lock(_ipMutex);

        if (_totalConnections >= _config.network.maxConnections)
        {
            return false;
        }

        auto &entry = _ipConnections[clientIP];
        if (entry.connCount >= _config.dos.maxConnsPerIP)
        {
            return false;
        }

        entry.connCount++;
        _totalConnections++;
        return true;
    }

    /**
     * @brief 连接关闭时更新计数器
     * @param clientIP  客户端 IP
     */
    void GateServer::OnConnectionClosed(const std::string &clientIP)
    {
        std::lock_guard lock(_ipMutex);
        auto            it = _ipConnections.find(clientIP);
        if (it != _ipConnections.end() && it->second.connCount > 0)
        {
            it->second.connCount--;
        }
        if (_totalConnections > 0)
        {
            _totalConnections--;
        }
    }

    // ── 超时检查 ──

    /**
     * @brief 启动 10s 间隔周期超时检查
     */
    void GateServer::StartTimeoutCheck()
    {
        _timeoutTimer =
            std::make_unique<asio::steady_timer>(_ioPool->GetNextContext(), std::chrono::seconds(10));

        _timeoutTimer->async_wait([this](const asio::error_code &ec) {
            if (ec || !_running.load(std::memory_order_acquire))
            {
                return;
            }
            CheckClientTimeouts();
            StartTimeoutCheck();
        });
    }

    /**
     * @brief 检查客户端心跳超时，超时则断开
     */
    void GateServer::CheckClientTimeouts()
    {
        uint32 timeoutSec = _config.heartbeat.clientTimeoutSec;

        // 收集超时 session（不能在持锁时 Close，会导致死锁）
        std::vector<uint32> toRemove;
        {
            std::lock_guard lock(_gateMutex);
            for (const auto &[sid, session] : _sessions)
            {
                if (session->IdleSeconds() >= timeoutSec)
                {
                    toRemove.push_back(sid);
                }
            }
        }

        for (uint32 sid : toRemove)
        {
            Log::Debug("GateServer: timeout session {}", sid);
            // Close 会触发 close handler → OnSessionDisconnect
            // 后者需要获取 _gateMutex，所以必须不能在这里持锁
            std::shared_ptr<GateSession> session;
            {
                std::lock_guard lock(_gateMutex);
                auto            it = _sessions.find(sid);
                if (it != _sessions.end())
                {
                    session = it->second;
                }
            }
            if (session)
            {
                session->Close();
            }
        }
    }

} // namespace MMO
