/**
 * @file TCPAcceptor.h
 * @brief 异步 TCP 接受器
 *
 * 监听端口，accept 新连接 → 交给 IOContextPool Round-Robin 分配的 io_context 线程
 * → 构造 TCPSocket 并回调上层。
 *
 * 帧协议模式：由构造参数指定 Framing，传递给每个 accept 的 TCPSocket。
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <asio/ip/tcp.hpp>

#include "Common/Core/Types.h"
#include "Common/Network/TCPSocket.h"

namespace MMO
{

class IOContextPool;

class TCPAcceptor
{
public:
    using AcceptHandler = std::function<void(std::shared_ptr<TCPSocket>)>;

    /**
     * @brief 构造接受器
     * @param pool    IOContextPool 引用（分配新连接的 io_context）
     * @param port    监听端口
     * @param framing 帧协议模式（默认 PacketHeader）
     */
    TCPAcceptor(IOContextPool& pool, uint16 port,
                Framing framing = Framing::PacketHeader);

    TCPAcceptor(const TCPAcceptor&) = delete;
    TCPAcceptor& operator=(const TCPAcceptor&) = delete;

    ~TCPAcceptor();

    /**
     * @brief 启动异步 accept 循环
     * @param onAccept  新连接回调（在 io_context 线程中触发）
     */
    void Start(AcceptHandler onAccept);

    // 停止接受新连接
    void Stop();

private:
    void DoAccept();

    IOContextPool&            _pool;
    asio::ip::tcp::acceptor   _acceptor;
    AcceptHandler             _onAccept;
    Framing                   _framing;
    bool                      _started = false;
};

} // namespace MMO
