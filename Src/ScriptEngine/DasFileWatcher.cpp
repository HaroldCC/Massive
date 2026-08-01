#include "DasFileWatcher.h"
#include "Common/Core/Types.h"
#include "Common/Log/Log.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>

namespace MMO
{
    static bool StatFile(const std::string &path, int64 &mtime, int64 &size)
    {
        std::error_code ec;
        auto            st = std::filesystem::last_write_time(path, ec);
        if (ec)
        {
            Log::Error("get file:{} last write time error:{}", path, ec.message());
            return false;
        }

        auto sz = std::filesystem::file_size(path, ec);
        if (ec)
        {
            Log::Error("get file size:{} fail:{}", path, ec.message());
            return false;
        }

        mtime = static_cast<int64>(st.time_since_epoch().count());
        size  = static_cast<int64>(sz);
        return true;
    }

    void DasFileWatcher::Start(std::vector<std::string> files, int64 pollMs, OnFileChanged cb)
    {
        if (_running)
        {
            return;
        }

        _pollMs        = pollMs;
        _fileChangedCB = std::move(cb);
        SetFiles(std::move(files));
        _running.store(true);

        _thread = std::jthread([this] {
            Loop();
        });
    }

    /**
     * @brief Swap后依赖集变化重建
     * @param files 文件
     */
    void DasFileWatcher::SetFiles(std::vector<std::string> files)
    {
        std::lock_guard lg(_mutex);
        _snapshot.clear();
        for (auto &f : files)
        {
            int64 m = 0;
            int64 s = 0;
            if (StatFile(f, m, s))
            {
                _snapshot[f] = {.mtime = m, .size = s};
            }
        }
    }

    void DasFileWatcher::Stop()
    {
        _running.store(false);
        if (_thread.joinable())
        {
            _thread.join();
        }
    }

    void DasFileWatcher::Loop()
    {
        while (_running.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(_pollMs));
            bool changed = false;
            {
                std::lock_guard lg(_mutex);
                for (auto &[path, snap] : _snapshot)
                {
                    int64 m = 0;
                    int64 s = 0;
                    if (StatFile(path, m, s) && (m != snap.mtime || s != snap.size))
                    {
                        snap.mtime = m;
                        snap.size  = s;
                        changed    = true;
                    }
                }
            }

            if (changed && nullptr != _fileChangedCB)
            {
                _fileChangedCB();
            }
        }
    }
} // namespace MMO