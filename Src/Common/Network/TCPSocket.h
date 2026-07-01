/**
 * @file TCPSocket.h
 * @brief 传输层 TCP socket 封装（异步收发 + 粘包拆包）
 *
 * shared_ptr 生命周期 + 异步回调，参照 TrinityCore Socket<T> 和 asio 官方范式。
 * 缓冲累积 + 批量切包：一次 async_read_some 处理多个完整包，摊薄 syscall 开销。
 *
 * 帧协议模式：
 *   PacketHeader — 默认。12B PacketHeader(length+msgID+sessionID) + body。
 *                  用于 Client<->Gate 的外部协议。
 *   LengthPrefix — 4B 总长度（含自身，大端）+ 裸数据。
 *                  用于 World<->Center 的内部 RPC。
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

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"
#include "Common/Network/PacketHeader.h"

namespace MMO
{

    /**
     * @brief RPC 消息类型枚举
     */
    enum class ERPCType : uint8;

    /**
     * @brief TCPSocket 帧协议模式
     */
    enum class EFraming : uint8
    {
        PacketHeader, // 12B PacketHeader + body（Client<->Gate）
        LengthPrefix, // 4B 总长度 + 裸数据（内部 RPC）
    };

    class TCPSocket : public std::enable_shared_from_this<TCPSocket>
    {
    public:
        /**
         * @brief 消息回调（收到完整包时调用）
         * @param msgID      消息 ID
         * @param sessionID  会话 ID
         * @param body       消息体指针
         * @param len        消息体长度
         */
        using MessageHandler =
            std::function<void(uint32 msgID, uint32 sessionID, const uint8 *body, size_t len)>;
        /**
         * @brief 连接关闭回调（对端断开 / 主动 Close / 错误）
         */
        using CloseHandler = std::function<void()>;

        /**
         * @brief 构造：接管已 accept 的 socket
         * @param socket  asio TCP socket（move）
         * @param framing 帧协议模式（默认 PacketHeader）
         */
        explicit TCPSocket(asio::ip::tcp::socket socket, EFraming framing = EFraming::PacketHeader);

        TCPSocket(const TCPSocket &)            = delete;
        TCPSocket &operator=(const TCPSocket &) = delete;

        ~TCPSocket();

        /**
         * @brief 启动异步读循环（在 io_context 线程中调用）
         */
        void Start();

        /**
         * @brief 异步发送数据（线程安全，所有 IO 线程均可调用）
         * @param data  ByteBuffer（Own 模式，内部 move）
         */
        void Send(ByteBuffer data);

        /**
         * @brief 关闭连接（幂等，可多次调用）
         */
        void Close();

        /**
         * @brief 延迟关闭：写队列排空后再关闭
         * @note 用于短连接场景（如 LoginServer 认证），Send 后调用此方法，
         *       等所有异步写都完成后才真正关闭连接。参考 TrinityCore DelayedClose 模式。
         */
        void DelayedClose();

        /**
         * @brief 发送后自动关闭（Send + DelayedClose 的语法糖）
         * @param data  ByteBuffer（Own 模式，内部 move）
         * @note 短连接场景快捷方式，内部分两步执行：
         *       1. Send(data)
         *       2. DelayedClose()
         */
        void SendThenClose(ByteBuffer data);

        /**
         * @brief 设置消息回调
         */
        void SetMessageHandler(MessageHandler handler)
        {
            _onMessage = std::move(handler);
        }

        /**
         * @brief 设置关闭回调
         */
        void SetCloseHandler(CloseHandler handler)
        {
            _onClose = std::move(handler);
        }

        /**
         * @brief 获取底层 socket 引用
         * @note 预留 TLS：将来 LowestLayer() 返回 ssl::stream 的 lowest_layer
         */
        asio::ip::tcp::socket &LowestLayer()
        {
            return _socket;
        }

        /**
         * @brief 获取帧协议模式
         */
        EFraming GetFraming() const
        {
            return _framing;
        }

        /**
         * @brief 是否为主动关闭（不含对端断开 / 网络错误）
         */
        bool IsClosed() const
        {
            return _closed.load(std::memory_order_acquire);
        }

    private:
        /**
         * @brief 异步读取数据（追加到 _readBuffer）
         */
        void DoRead();
        /**
         * @brief PacketHeader 模式拆包
         */
        void ProcessReadBuffer();
        /**
         * @brief LengthPrefix 模式拆包
         */
        void ProcessLengthPrefixed();
        /**
         * @brief 链式异步写入(发送队列)
         */
        void DoWrite();
        /**
         * @brief 关闭 socket + 触发 OnClose
         */
        void DoClose();
        /**
         * @brief 统一错误处理
         */
        void HandleError(const asio::error_code &ec);

        asio::ip::tcp::socket _socket;
        EFraming               _framing;

        // 读缓冲
        std::vector<uint8>      _readBuffer;                  // 累积缓冲
        static constexpr size_t kMaxPacketSize = 1024 * 1024; // 1 MiB
        // 写缓冲
        std::mutex             _writeMutex;
        std::deque<ByteBuffer> _writeQueue;
        bool                   _writing = false;
        bool                   _pendingClose = false; // DelayedClose 标记：队列排空后关闭

        // 状态
        std::atomic<bool> _closed {false};
        std::atomic<bool> _started {false};

        // 回调
        MessageHandler _onMessage;
        CloseHandler   _onClose;

        // 拆包中间状态
        static constexpr size_t kHeaderSize = sizeof(PacketHeader);
    };

} // namespace MMO
