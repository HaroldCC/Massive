/**
 * @file TCPAcceptor.cpp
 * @brief 异步 TCP 接受器实现
 */

#include "Common/Network/TCPAcceptor.h"
#include "Common/Network/IOContextPool.h"
#include "Common/Network/TCPSocket.h"
#include "Common/Log/Log.h"

namespace MMO
{

TCPAcceptor::TCPAcceptor(IOContextPool& pool, uint16 port)
    : _pool(pool)
    , _acceptor(pool.GetNextContext(), asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port))
{
}

TCPAcceptor::~TCPAcceptor()
{
    Stop();
}

void TCPAcceptor::Start(AcceptHandler onAccept)
{
    if (_started)
    {
        return;
    }
    _started = true;
    _onAccept = std::move(onAccept);
    DoAccept();
}

void TCPAcceptor::Stop()
{
    asio::error_code ec;
    _acceptor.close(ec);
    _started = false;
}

void TCPAcceptor::DoAccept()
{
    if (!_started)
    {
        return;
    }

    // 从池中取下一个 io_context 给新连接
    auto& ctx = _pool.GetNextContext();

    _acceptor.async_accept(ctx,
        [this](const asio::error_code& ec, asio::ip::tcp::socket peer)
        {
            if (ec)
            {
                Log::Error("TCPAcceptor: accept error: {}", ec.message());
                // 继续监听（如 acceptor 未关闭）
                if (_started)
                {
                    DoAccept();
                }
                return;
            }

            Log::Info("TCPAcceptor: new connection from {}", peer.remote_endpoint().address().to_string());

            auto sock = std::make_shared<TCPSocket>(std::move(peer));
            if (_onAccept)
            {
                _onAccept(std::move(sock));
            }

            // 继续接受下一个连接
            DoAccept();
        });
}

} // namespace MMO
