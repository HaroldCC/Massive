#pragma once

#include "Common/Core/Types.h"

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace MMO
{
    class DasFileWatcher
    {
    public:
        using OnFileChanged = std::function<void()>;

        void Start(std::vector<std::string> files, int64 pollMs, OnFileChanged cb);

        /**
         * @brief Swap后依赖集变化重建
         * @param files 文件
         */
        void SetFiles(std::vector<std::string> files);

        void Stop();

    private:
        void Loop();

    private:
        struct Snapshot
        {
            int64 mtime;
            int64 size;
        };

        std::unordered_map<std::string, Snapshot> _snapshot;
        std::mutex                                _mutex;
        std::jthread                              _thread;
        std::atomic<bool>                         _running {false};
        int64                                     _pollMs = 500;
        OnFileChanged                             _fileChangedCB;
    };
} // namespace MMO