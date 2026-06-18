/**
 * @file DBWorkerPool.h
 * @brief libpq 异步 Worker 线程池
 *
 * 所有需要 DB 的进程共用（Login/World/Social）。
 * Worker 线程独立运行，阻塞式 libpq 调用。
 * 结果通过 MPSC Queue 回调到逻辑线程。
 *
 * 使用示例：
 * @code
 *   DBWorkerPool::Init(3, "host=127.0.0.1 port=6432 dbname=massive");
 *   DBWorkerPool::Instance().AsyncQuery("SELECT * FROM players WHERE player_id = $1",
 *       {DBValue(1001)},
 *       [](const DBResult& res) { ... });
 * @endcode
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Common/DB/Types.h"
#include "Common/Queue/MPSCQueue.h"

namespace MMO::DB
{

// 异步 DB 任务
struct DBTask
{
    std::string                                 sql;
    std::vector<DBValue>                        params;
    std::function<void(const DBResult&)>        callback;
};

// 已完成回调包装
struct DBCallback
{
    std::function<void(const DBResult&)>        callback;
    std::unique_ptr<DBResult>                   result;
};

/**
 * @brief DBWorkerPool — libpq 异步 Worker 线程池
 *
 * 单例模式，所有进程共用。
 * 逻辑线程每 Tick 调用 ProcessCallbacks() 处理已完成查询的回调。
 */
class DBWorkerPool
{
public:
    /**
     * @brief 初始化连接池
     * @param workerCount  Worker 线程数（建议 3-5）
     * @param connString   libpq 连接字符串
     * @return true 初始化成功
     */
    static bool Init(int workerCount, const std::string& connString);

    // 获取单例
    static DBWorkerPool& Instance();

    /**
     * @brief 异步执行参数化 SQL
     * @param sql      参数化 SQL（$1, $2 占位符）
     * @param params   参数列表
     * @param callback 逻辑线程回调
     */
    void AsyncQuery(
        std::string sql,
        std::vector<DBValue> params,
        std::function<void(const DBResult&)> callback);

    // 逻辑线程每 Tick 调用，处理已完成查询的回调
    void ProcessCallbacks();

    // 是否已初始化
    bool IsInitialized() const { return !_workers.empty(); }

    // 停止所有 Worker 并清理
    void Shutdown();

private:
    DBWorkerPool() = default;
    ~DBWorkerPool();
    DBWorkerPool(const DBWorkerPool&) = delete;
    DBWorkerPool& operator=(const DBWorkerPool&) = delete;

    // 单个 Worker 线程
    struct Worker
    {
        void*               conn = nullptr;       // PGconn*
        std::thread         thread;
        MPSCQueue<DBTask>   taskQueue;
        std::atomic<bool>   stop{false};
    };

    std::vector<std::unique_ptr<Worker>> _workers;
    MPSCQueue<DBCallback>                _completedCallbacks;
    bool                                 _shutdown = false;
};

} // namespace MMO::DB
