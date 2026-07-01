/**
 * @file TCPSocket.cpp
 * @brief TCP socket 异步收发 + 粘包拆包实现
 */

#include "Common/Network/TCPSocket.h"
#include "Common/Log/Log.h"

#include <asio/read.hpp>
#include <asio/write.hpp>

namespace MMO
{

    TCPSocket::TCPSocket(asio::ip::tcp::socket socket, EFraming framing)
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
                _writing  = true;
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

    /**
     * @brief 延迟关闭：写队列排空后再关闭
     * @note 用于短连接场景（如 LoginServer 认证），Send 后调用此方法，
     *       等所有异步写都完成后才真正关闭连接。参考 TrinityCore DelayedClose 模式。
     */
    void TCPSocket::DelayedClose()
    {
        std::unique_lock lock(_writeMutex);
        if (_writeQueue.empty())
        {
            lock.unlock();
            DoClose();
        }
        else
        {
            _pendingClose = true;
        }
    }

    void TCPSocket::SendThenClose(ByteBuffer data)
    {
        Send(std::move(data));
        DelayedClose();
    }

    void TCPSocket::DoRead()
    {
        auto self = shared_from_this();

        constexpr size_t kMaxReadBuffer = kMaxPacketSize * 4;
        if (_readBuffer.size() >= kMaxReadBuffer)
        {
            Log::Error("TCPSocket: Read buffer exhausted ({} bytes), closing", _readBuffer.size());
            DoClose();
            return;
        }

        // reserve() 仅分配不构造 → async_read_some 写入未初始化内存
        // resize() 先构造 → async_read_some 写入已初始化 → resize() 裁到实际大小
        static constexpr size_t kReadChunk = 4096;
        size_t oldSize = _readBuffer.size();

        if (_readBuffer.capacity() < oldSize + kReadChunk)
        {
            _readBuffer.reserve(oldSize + kReadChunk);
        }
        _readBuffer.resize(oldSize + kReadChunk);

        _socket.async_read_some(asio::buffer(_readBuffer.data() + oldSize, kReadChunk),
                                [self, oldSize](const asio::error_code &ec, size_t bytesTransferred) {
                                    if (ec)
                                    {
                                        self->HandleError(ec);
                                        return;
                                    }

                                    self->_readBuffer.resize(oldSize + bytesTransferred);

                                    if (self->_framing == EFraming::LengthPrefix)
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
            auto   headerBuf = ByteBuffer::Wrap(_readBuffer.data(), kHeaderSize);
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
            auto   lenBuf   = ByteBuffer::Wrap(_readBuffer.data(), kLenFieldSize);
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

        std::unique_lock lock(_writeMutex);
        if (_writeQueue.empty())
        {
            _writing = false;
            // 队列排空 + 标记延迟关闭 → 关闭连接
            if (_pendingClose)
            {
                _pendingClose = false;
                lock.unlock();
                DoClose();
            }
            return;
        }

        auto &buf = _writeQueue.front();
        asio::async_write(_socket,
                          asio::buffer(buf.Data(), buf.Size()),
                          [self](const asio::error_code &ec, size_t) {
                              if (ec)
                              {
                                  self->HandleError(ec);
                                  return;
                              }

                              // 在持锁状态下 pop_front，然后释放锁再递归 DoWrite
                              // std::mutex 不可重入，不能在持锁时调用 DoWrite
                              {
                                  std::unique_lock lk(self->_writeMutex);
                                  self->_writeQueue.pop_front();
                              }
                              self->DoWrite(); // 链式写下一个
                          });
    }

    void TCPSocket::DoClose()
    {
        if (_closed.exchange(true, std::memory_order_acq_rel))
        {
            return; // 幂等
        }

        asio::error_code ec;
        _socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        _socket.close(ec);

        if (_onClose)
        {
            _onClose();
        }
    }

    void TCPSocket::HandleError(const asio::error_code &ec)
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
