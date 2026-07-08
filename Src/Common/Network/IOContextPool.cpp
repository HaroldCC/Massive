/**
 * @file IOContextPool.cpp
 * @brief N 线程 asio::io_context 线程池实现
 */

#include "Common/Network/IOContextPool.h"

namespace MMO
{

    IOContextPool::IOContextPool(std::size_t size) : _ioContexts(size)
    {
        _works.reserve(size);
        for (auto &ctx : _ioContexts)
        {
            _works.emplace_back(ctx.get_executor());
        }
    }

    IOContextPool::~IOContextPool()
    {
        Stop();
    }

    void IOContextPool::Start()
    {
        if (_started)
        {
            return;
        }
        _started = true;

        _threads.reserve(_ioContexts.size());
        for (auto &ctx : _ioContexts)
        {
            _threads.emplace_back([&ctx] {
                ctx.run();
            });
        }
    }

    void IOContextPool::Wait()
    {
        _mainCtx.run(); // 主线程阻塞，直到 Stop() 调用 _mainCtx.stop()
    }

    asio::io_context &IOContextPool::GetNextContext()
    {
        auto index = _nextIndex.fetch_add(1, std::memory_order_relaxed);
        return _ioContexts[index % _ioContexts.size()];
    }

    void IOContextPool::Stop()
    {
        // 停主线程 io_context（让 Wait() 返回）
        _mainCtx.stop();

        for (auto &ctx : _ioContexts)
        {
            ctx.stop();
        }

        for (auto &t : _threads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
        _threads.clear();
        _started = false;
    }

} // namespace MMO
