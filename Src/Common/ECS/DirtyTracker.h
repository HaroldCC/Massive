/**
 * @file DirtyTracker.h
 * @brief Per-Component 脏标记模板
 *
 * 用于网络复制和 DB 写回——系统修改某 entity 的组件后，
 * 由系统显式调用 MarkDirty()，Tick 结束时 Drain() 集中处理。
 */
#pragma once

#include <unordered_set>

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief Per-Component 脏标记
     * @tparam T  组件类型（每个组件类型独立的脏标记集）
     */
    template <typename T>
    class DirtyTracker
    {
    public:
        void Mark(uint32 entityID)
        {
            _dirty.insert(entityID);
        }

        void Clear(uint32 entityID)
        {
            _dirty.erase(entityID);
        }

        bool IsDirty(uint32 entityID) const
        {
            return _dirty.contains(entityID);
        }

        /**
         * @brief 获取所有脏 entity 并清空
         * @return 脏 entity 集合
         */
        std::unordered_set<uint32> Drain()
        {
            std::unordered_set<uint32> result;
            result.swap(_dirty);
            return result;
        }

        size_t Count() const
        {
            return _dirty.size();
        }

    private:
        std::unordered_set<uint32> _dirty;
    };

} // namespace MMO
