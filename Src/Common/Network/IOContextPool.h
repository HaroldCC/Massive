/**
 * @file IOContextPool.h
 * @brief N 个独立 asio::io_context 的线程池
 *
 * Round-Robin 分配连接，同一连接始终在同一线程（零锁竞争）。
 * 所有 5 种进程都使用相同的 IOContextPool 组件。
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/executor_work_guard.hpp>

// Asio 需要 Windows 平台宏
#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601
    #endif
#endif

namespace MMO
{

    /**
     * @brief N 个独立 asio::io_context 的线程池
     *
     * 每个 io_context 运行于独立线程，Round-Robin 分配连接。
     */
    class IOContextPool
    {
    public:
        /**
         * @brief 构造 IOContextPool
         * @param size  io_context 数量，默认 hardware_concurrency()
         */
        explicit IOContextPool(std::size_t size = std::thread::hardware_concurrency());

        IOContextPool(const IOContextPool &)            = delete;
        IOContextPool &operator=(const IOContextPool &) = delete;

        ~IOContextPool();

        // 启动所有 io_context 线程
        void Start();

        /**
         * @brief Round-Robin 获取下一个 io_context
         * @return asio::io_context&
         */
        asio::io_context &GetNextContext();

        // 停止所有 io_context 并 join 线程
        void Stop();

        // io_context 数量
        std::size_t Size() const
        {
            return _ioContexts.size();
        }

    private:
        using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

        std::vector<asio::io_context> _ioContexts;
        std::vector<WorkGuard>        _works;
        std::vector<std::thread>      _threads;
        std::atomic<std::size_t>      _nextIndex {0};
        bool                          _started = false;
    };

} // namespace MMO
