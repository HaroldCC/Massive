/**
 * @file TCPSocket.cpp
 * @brief TCP socket 异步收发 + 粘包拆包实现
 */

#include "Common/Network/TCPSocket.h"
#include "Common/Log/Log.h"

#include <asio/read.hpp>

namespace MMO
{

TCPSocket::TCPSocket(asio::ip::tcp::socket socket, Framing framing)
    : _socket(std::move(socket))
    , _framing(framing)
{
}

TCPSocket::~TCPSocket()
{
    Close();
}

void TCPSocket::Start()
{
    if (_started.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    _readBuffer.reserve(kMaxPacketSize);
    DoRead();
}

void TCPSocket::Send(ByteBuffer data)
{
    if (_closed.load(std::memory_order_acquire))
    {
        Log::Warn("TCPSocket: Send on closed socket");
        return;
    }

    bool needWrite = false;
    {
        std::lock_guard lock(_writeMutex);
        _writeQueue.push_back(std::move(data));
        if (!_writing)
        {
            _writing = true;
            needWrite = true;
        }
    }
    if (needWrite)
    {
        DoWrite();
    }
}

void TCPSocket::Close()
{
    DoClose();
}

void TCPSocket::DoRead()
{
    auto self = shared_from_this();

    // 确保读缓冲有空间
    constexpr size_t kMaxReadBuffer = kMaxPacketSize * 4;
    if (_readBuffer.capacity() < kMaxPacketSize)
    {
        _readBuffer.reserve(kMaxPacketSize);
    }
    if (_readBuffer.size() >= kMaxReadBuffer)
    {
        Log::Error("TCPSocket: Read buffer exhausted ({} bytes), closing", _readBuffer.size());
        DoClose();
        return;
    }

    // 至少有 4 KiB 空间可读
    if (_readBuffer.capacity() - _readBuffer.size() < 4096)
    {
        size_t newCap = std::min(_readBuffer.capacity() + kMaxPacketSize, kMaxReadBuffer);
        _readBuffer.reserve(newCap);
    }

    _socket.async_read_some(asio::buffer(_readBuffer.data() + _readBuffer.size(),
                                         _readBuffer.capacity() - _readBuffer.size()),
        [self](const asio::error_code& ec, size_t bytesTransferred)
        {
            if (ec)
            {
                self->HandleError(ec);
                return;
            }

            self->_readBuffer.resize(self->_readBuffer.size() + bytesTransferred);

            if (self->_framing == Framing::LengthPrefix)
            {
                self->ProcessLengthPrefixed();
            }
            else
            {
                self->ProcessReadBuffer();
            }

            self->DoRead();
        });
}

void TCPSocket::ProcessReadBuffer()
{
    while (_readBuffer.size() >= kHeaderSize)
    {
        auto headerBuf = ByteBuffer::Wrap(_readBuffer.data(), kHeaderSize);
        uint32 length    = headerBuf.ReadUint32();
        uint32 msgID     = headerBuf.ReadUint32();
        uint32 sessionID = headerBuf.ReadUint32();

        if (length < kHeaderSize || length > kMaxPacketSize)
        {
            Log::Error("TCPSocket: Invalid packet length {}, closing", length);
            DoClose();
            return;
        }

        if (_readBuffer.size() < length)
        {
            break;
        }

        size_t bodyLen = length - kHeaderSize;
        if (_onMessage)
        {
            _onMessage(msgID, sessionID, _readBuffer.data() + kHeaderSize, bodyLen);
        }

        _readBuffer.erase(_readBuffer.begin(), _readBuffer.begin() + length);
    }

    if (_readBuffer.size() > kMaxPacketSize * 2)
    {
        Log::Error("TCPSocket: Read buffer overflow ({} bytes), closing", _readBuffer.size());
        DoClose();
    }
}

void TCPSocket::ProcessLengthPrefixed()
{
    static constexpr size_t kLenFieldSize = 4; // uint32, big-endian

    while (_readBuffer.size() >= kLenFieldSize)
    {
        // 读取 4B 总长度
        auto lenBuf = ByteBuffer::Wrap(_readBuffer.data(), kLenFieldSize);
        uint32 totalLen = lenBuf.ReadUint32();

        if (totalLen < kLenFieldSize || totalLen > kMaxPacketSize)
        {
            Log::Error("TCPSocket: Invalid length-prefixed packet length {}, closing", totalLen);
            DoClose();
            return;
        }

        if (_readBuffer.size() < totalLen)
        {
            break; // 等待更多数据
        }

        // totalLen 包含自身 4B + 后续数据
        // 跳过 4B 长度字段，剩余为 RPCHeader + protobuf body
        size_t bodyLen = totalLen - kLenFieldSize;
        // msgID/sessionID 在 RPCHeader 中，不是 PacketHeader 格式
        // 统一用 msgID=0, sessionID=0，由 RPC header 解析
        if (_onMessage)
        {
            _onMessage(0, 0, _readBuffer.data() + kLenFieldSize, bodyLen);
        }

        _readBuffer.erase(_readBuffer.begin(), _readBuffer.begin() + totalLen);
    }

    if (_readBuffer.size() > kMaxPacketSize * 2)
    {
        Log::Error("TCPSocket: Length-read buffer overflow ({} bytes), closing", _readBuffer.size());
        DoClose();
    }
}

void TCPSocket::DoWrite()
{
    auto self = shared_from_this();

    std::lock_guard lock(_writeMutex);
    if (_writeQueue.empty())
    {
        _writing = false;
        return;
    }

    auto& buf = _writeQueue.front();
    asio::async_write(_socket, asio::buffer(buf.Data(), buf.Size()),
        [self](const asio::error_code& ec, size_t)
        {
            if (ec)
            {
                self->HandleError(ec);
                return;
            }

            std::lock_guard lk(self->_writeMutex);
            self->_writeQueue.pop_front();
            self->DoWrite();  // 链式写下一个
        });
}

void TCPSocket::DoClose()
{
    if (_closed.exchange(true, std::memory_order_acq_rel))
    {
        return;  // 幂等
    }

    asio::error_code ec;
    _socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    _socket.close(ec);

    if (_onClose)
    {
        _onClose();
    }
}

void TCPSocket::HandleError(const asio::error_code& ec)
{
    if (ec == asio::error::eof)
    {
        DoClose();
    }
    else if (ec == asio::error::operation_aborted)
    {
        // 主动关闭触发，不需要额外操作
    }
    else
    {
        Log::Error("TCPSocket: asio error: {}", ec.message());
        DoClose();
    }
}

} // namespace MMO
