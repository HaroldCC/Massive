#pragma once

#include <unordered_set>

#include "Common/ECS/EntityID.h"

namespace MMO::ECS
{

    /**
     * @brief 活跃实体集——AOI 内或正在模拟的实体
     *
     * 用途：
     *   - Movement 只积分活跃实体（排除 DormantTag）
     *   - 复制只打包活跃实体
     *   - 脚本事件只派发活跃实体相关
     *
     * 实现：观察者（玩家）可见并集 + 显式标记的常驻实体。
     * 键为 EntityIndex（EnTT 内部索引），不是对外 EntityID。
     */
    class ActiveSet
    {
    public:
        void Add(EntityIndex index)
        {
            _active.insert(index);
        }

        void Remove(EntityIndex index)
        {
            _active.erase(index);
        }

        bool Contains(EntityIndex index) const
        {
            return _active.contains(index);
        }

        /**
         * @brief 由 AOI 事件流更新：enter 加，leave 减（玩家自身恒活跃）
         */
        void ApplyAOIEvents(const std::vector<std::pair<EntityIndex, EntityIndex>> &enters,
                            const std::vector<std::pair<EntityIndex, EntityIndex>> &leaves)
        {
            for (auto &[obs, idx] : enters)
            {
                _active.insert(idx);
            }

            for (auto &[obs, idx] : leaves)
            {
                _active.erase(idx);
            }
        }

        size_t Count() const
        {
            return _active.size();
        }

        void Clear()
        {
            _active.clear();
        }

        // 遍历
        auto begin() const
        {
            return _active.begin();
        }

        auto end() const
        {
            return _active.end();
        }

    private:
        std::unordered_set<EntityIndex> _active;
    };

} // namespace MMO::ECS