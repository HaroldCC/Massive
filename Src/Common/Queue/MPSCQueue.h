/**
 * @file MPSCQueue.h
 * @brief 无锁 MPSC 队列（moodycamel::ConcurrentQueue 薄封装）
 *
 * 多生产者 Enqueue → 单消费者 DrainAll。
 * 委托给 moodycamel::ConcurrentQueue，保持自有 API 风格。
 */
#pragma once

#include <vector>

#include <concurrentqueue.h>

namespace MMO
{

    /**
     * @brief 多生产者单消费者无锁队列
     *
     * @tparam T  队列元素类型
     */
    template <typename T>
    class MPSCQueue
    {
    public:
        /** @brief 生产者端入队（多线程安全） */
        void Enqueue(T item)
        {
            _queue.enqueue(std::move(item));
        }

        /** @brief 消费者端批量 drain（单线程调用） */
        void DrainAll(std::vector<T> &out)
        {
            size_t count = _queue.size_approx();
            out.resize(count);
            size_t drained = _queue.try_dequeue_bulk(out.data(), count);
            out.resize(drained);
        }

        /** @brief 近似队列深度 */
        size_t SizeApprox() const
        {
            return _queue.size_approx();
        }

    private:
        moodycamel::ConcurrentQueue<T> _queue;
    };

} // namespace MMO
