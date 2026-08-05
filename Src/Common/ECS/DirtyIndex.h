#pragma once

#include <vector>

#include "Common/ECS/EntityID.h"

namespace MMO::ECS
{

    /**
     * @brief 按组件类型分桶的脏实体标记（带 O(1) 去重）
     *
     * _marked 位图（EntityIndex → bool）保证每实体每帧至多进一次桶；
     * _dirty 列表保序输出。
     */
    template <typename T>
    class DirtyIndex
    {
    public:
        void Mark(EntityIndex index)
        {
            if (index.raw >= _marked.size())
            {
                _marked.resize(index.raw + 1, false);
            }
            if (!_marked[index.raw])
            {
                _marked[index.raw] = true;
                _dirty.push_back(index);
            }
        }

        std::vector<EntityIndex> Drain()
        {
            for (EntityIndex i : _dirty)
            {
                _marked[i.raw] = false;
            }
            std::vector<EntityIndex> result;
            result.swap(_dirty);
            return result;
        }

        size_t Count() const
        {
            return _dirty.size();
        }

    private:
        std::vector<bool>        _marked;
        std::vector<EntityIndex> _dirty;
    };

} // namespace MMO::ECS