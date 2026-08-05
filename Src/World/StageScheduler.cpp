#include "StageScheduler.h"
#include "Common/Log/Log.h"

namespace MMO
{

    void StageScheduler::RegisterSystem(EStage stage, std::string_view name, SystemFunc fn)
    {
        const auto idx = static_cast<size_t>(stage);
        if (idx >= _stages.size() || !fn)
        {
            Log::Error("StageScheduler: invalid register stage={} name={}", idx, name);
            return;
        }
        _stages[idx].push_back({.name = std::string(name), .func = std::move(fn)});
    }

    void StageScheduler::RunStage(EStage stage, float dt)
    {
        const auto idx = static_cast<size_t>(stage);
        if (idx >= _stages.size())
        {
            return;
        }

        for (auto &entry : _stages[idx])
        {
            entry.func(dt);
        }
    }

    void StageScheduler::RunAllStage(float dt)
    {
        for (size_t i = 0; i < _stages.size(); ++i)
        {
            RunStage(static_cast<EStage>(i), dt);
        }
    }

    size_t StageScheduler::GetSystemCount(EStage stage) const
    {
        return _stages[static_cast<size_t>(stage)].size();
    }

} // namespace MMO