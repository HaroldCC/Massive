/**
 * @file TCPSocket.h
 * @brief 传输层 TCP socket 封装（异步收发 + 粘包拆包）
 *
 * shared_ptr 生命周期 + 异步回调，参照 TrinityCore Socket<T> 和 asio 官方范式。
 * 缓冲累积 + 批量切包：一次 async_read_some 处理多个完整包，摊薄 syscall 开销。
 */
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"
#include "Common/Network/PacketHeader.h"

namespace MMO
{

class TCPSocket : public std::enable_shared_from_this<TCPSocket>
{
public:
    // 收到完整包的回调
    using MessageHandler = std::function<void(uint32 msgID, uint32 sessionID,
                                              const uint8* body, size_t len)>;
    // 连接关闭回调（对端断开 / 主动 Close / 错误）
    using CloseHandler   = std::function<void()>;

    /**
     * @brief 构造：接管已 accept 的 socket
     * @param socket  asio TCP socket（move）
     */
    explicit TCPSocket(asio::ip::tcp::socket socket);

    TCPSocket(const TCPSocket&) = delete;
    TCPSocket& operator=(const TCPSocket&) = delete;

    ~TCPSocket();

    // 启动异步读循环（在 io_context 线程中调用）
    void Start();

    /**
     * @brief 异步发送数据（线程安全，所有 IO 线程均可调用）
     * @param data  ByteBuffer（Own 模式，内部 move）
     */
    void Send(ByteBuffer data);

    // 关闭连接（幂等，可多次调用）
    void Close();

    // 设置回调
    void SetMessageHandler(MessageHandler handler) { _onMessage = std::move(handler); }
    void SetCloseHandler(CloseHandler handler)     { _onClose = std::move(handler); }

    // 底层 socket 引用（获取远程地址等）
    asio::ip::tcp::socket& Socket() { return _socket; }

    // 是否为主动关闭（不含对端断开 / 网络错误）
    bool IsClosed() const { return _closed.load(std::memory_order_acquire); }

private:
    void DoRead();                               // async_read → 追加到 _readBuffer
    void ProcessReadBuffer();                     // 循环切出完整包 → OnMessage
    void DoWrite();                               // 链式 async_write
    void DoClose();                               // 关闭 socket + 触发 OnClose
    void HandleError(const asio::error_code& ec); // 统一错误处理

    asio::ip::tcp::socket _socket;

    // 读缓冲
    std::vector<uint8> _readBuffer;    // 累积缓冲
    static constexpr size_t kMaxPacketSize = 1024 * 1024;  // 1 MiB

    // 写缓冲
    std::mutex            _writeMutex;
    std::deque<ByteBuffer> _writeQueue;
    bool                   _writing = false;

    // 状态
    std::atomic<bool> _closed{false};
    std::atomic<bool> _started{false};

    // 回调
    MessageHandler _onMessage;
    CloseHandler   _onClose;

    // 拆包中间状态
    static constexpr size_t kHeaderSize = sizeof(PacketHeader);
};

} // namespace MMO
