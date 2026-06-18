/**
 * @file DBWorkerPool.cpp
 * @brief libpq 异步 Worker 线程池实现
 */

#include "Common/DB/DBWorkerPool.h"
#include "Common/Log/Log.h"

#include <libpq-fe.h>

namespace MMO::DB
{

DBWorkerPool::~DBWorkerPool()
{
    Shutdown();
}

bool DBWorkerPool::Init(int workerCount, const std::string& connString)
{
    auto& inst = Instance();
    if (!inst._workers.empty())
    {
        return true;
    }

    inst._workers.reserve(static_cast<size_t>(workerCount));
    for (int i = 0; i < workerCount; ++i)
    {
        auto worker = std::make_unique<Worker>();

        worker->conn = PQconnectdb(connString.c_str());
        auto* conn = static_cast<PGconn*>(worker->conn);
        if (PQstatus(conn) != CONNECTION_OK)
        {
            Log::Error("DBWorkerPool Worker {} connect failed: {}", i, PQerrorMessage(conn));
            PQfinish(conn);
            return false;
        }

        auto* workerPtr = worker.get();
        auto& callbacks = inst._completedCallbacks;

        worker->thread = std::thread([workerPtr, &callbacks]()
        {
            auto* conn = static_cast<PGconn*>(workerPtr->conn);
            while (!workerPtr->stop.load(std::memory_order_acquire))
            {
                std::vector<DBTask> batch;
                workerPtr->taskQueue.DrainAll(batch);

                for (auto& task : batch)
                {
                    int nParams = static_cast<int>(task.params.size());
                    std::vector<const char*> values(static_cast<size_t>(nParams));
                    std::vector<int> lengths(static_cast<size_t>(nParams));
                    std::vector<int> formats(static_cast<size_t>(nParams));

                    for (int j = 0; j < nParams; ++j)
                    {
                        values[j]  = task.params[j].CStr();
                        lengths[j] = task.params[j].Length();
                        formats[j] = task.params[j].Format();
                    }

                    auto* pgResult = PQexecParams(conn,
                        task.sql.c_str(),
                        nParams,
                        nullptr,
                        values.data(),
                        lengths.data(),
                        formats.data(),
                        0);

                    auto result = std::make_unique<DBResult>(pgResult);
                    if (task.callback)
                    {
                        callbacks.Enqueue(DBCallback{
                            std::move(task.callback),
                            std::move(result)
                        });
                    }
                }

                if (batch.empty())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        });

        inst._workers.push_back(std::move(worker));
    }

    return true;
}

DBWorkerPool& DBWorkerPool::Instance()
{
    static DBWorkerPool inst;
    return inst;
}

void DBWorkerPool::AsyncQuery(
    std::string sql,
    std::vector<DBValue> params,
    std::function<void(const DBResult&)> callback)
{
    if (_workers.empty())
    {
        Log::Error("DBWorkerPool AsyncQuery called before Init");
        return;
    }

    static std::atomic<size_t> nextWorker{0};
    auto index = nextWorker.fetch_add(1, std::memory_order_relaxed) % _workers.size();
    auto& worker = _workers[index];

    DBTask task;
    task.sql = std::move(sql);
    task.params = std::move(params);
    task.callback = std::move(callback);
    worker->taskQueue.Enqueue(std::move(task));
}

void DBWorkerPool::ProcessCallbacks()
{
    std::vector<DBCallback> callbacks;
    _completedCallbacks.DrainAll(callbacks);

    for (auto& cb : callbacks)
    {
        if (cb.callback && cb.result)
        {
            cb.callback(*cb.result);
        }
    }
}

void DBWorkerPool::Shutdown()
{
    if (_shutdown)
    {
        return;
    }
    _shutdown = true;

    for (auto& worker : _workers)
    {
        if (worker)
        {
            worker->stop.store(true, std::memory_order_release);
            if (worker->thread.joinable())
            {
                worker->thread.join();
            }
            if (worker->conn)
            {
                PQfinish(static_cast<PGconn*>(worker->conn));
                worker->conn = nullptr;
            }
        }
    }
    _workers.clear();
}

} // namespace MMO::DB
