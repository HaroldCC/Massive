/**
 * @file TimingWheel.cpp
 * @brief 四级时间轮定时器实现
 */

#include "Common/Timer/TimingWheel.h"
#include "Common/Log/Log.h"

namespace MMO
{

    /**
 * @brief 全局 TimerID 生成器（永不回绕）
 */
    std::atomic<TimingWheel::TimerID> TimingWheel::_nextTimerID {1};

    /**
 * @brief 生成唯一 TimerID
 */
    TimingWheel::TimerID TimingWheel::GenTimerID()
    {
        return _nextTimerID.fetch_add(1, std::memory_order_relaxed);
    }

    /**
 * @brief 构造时间轮，初始化所有槽位为空
 */
    TimingWheel::TimingWheel()
    {
        for (int i = 0; i < kLevels; ++i)
        {
            _currentSlot[i] = 0;
            for (int j = 0; j < kSlots; ++j)
            {
                _wheels[i][j] = nullptr;
            }
        }
    }

    /**
 * @brief 析构时间轮，清理所有剩余节点和空闲池
 */
    TimingWheel::~TimingWheel()
    {
        for (int i = 0; i < kLevels; ++i)
        {
            for (int j = 0; j < kSlots; ++j)
            {
                Node *cur = _wheels[i][j];
                while (cur)
                {
                    Node *next = cur->next;
                    delete cur;
                    cur = next;
                }
            }
        }

        ClearFreeList();
    }

    /**
     * @brief 从对象池分配节点
     * @param id      TimerID
     * @param rounds  剩余轮数
     * @param cb      到期回调
     * @return 节点指针（池中有空闲则复用，否则 new）
     */
    TimingWheel::Node *TimingWheel::AllocNode(TimerID id, int rounds, Callback cb)
    {
        Node *node = nullptr;

        if (_freeList)
        {
            node                  = _freeList;
            _freeList             = _freeList->next;
            node->id              = id;
            node->remainingRounds = rounds;
            node->callback        = std::move(cb);
            node->next            = nullptr;
            node->cancelled       = false;
        }
        else
        {
            node = new Node {id, rounds, std::move(cb), nullptr, false};
        }

        return node;
    }

    /**
 * @brief 将节点归还到对象池
 */
    void TimingWheel::FreeNode(Node *node)
    {
        node->callback = {};
        node->next     = _freeList;
        _freeList      = node;
    }

    /**
 * @brief 释放对象池中所有节点
 */
    void TimingWheel::ClearFreeList()
    {
        while (_freeList)
        {
            Node *next = _freeList->next;
            delete _freeList;
            _freeList = next;
        }
    }

    /**
     * @brief O(1) 插入定时器
     * @param delay  延迟时间（上限 3h，超出则 clamp）
     * @param cb     到期回调
     * @return 唯一 TimerID
     */
    TimingWheel::TimerID TimingWheel::Schedule(std::chrono::milliseconds delay, Callback cb)
    {
        if (delay > kMaxDelay)
        {
            delay = kMaxDelay;
        }

        TimerID id = GenTimerID();

        // 确定层级——从低到高，找到第一个 slotOffset < kSlots 的层级
        int level      = 0;
        int slotOffset = 0;
        for (int i = 0; i < kLevels; ++i)
        {
            slotOffset = static_cast<int>(delay / kTickSizes[i]);
            if (slotOffset < kSlots)
            {
                level = i;
                break;
            }
        }
        if (level >= kLevels)
        {
            level      = kLevels - 1;
            slotOffset = kSlots - 1; // 兜底：clamp 到最大槽
        }

        int targetSlot = (_currentSlot[level] + slotOffset) % kSlots;
        int rounds     = slotOffset / kSlots;

        Node *node = AllocNode(id, rounds, std::move(cb));
        InsertToWheel(level, targetSlot, node);
        _idMap[id] = node;

        return id;
    }

    /**
 * @brief 节点插入槽位链表头部
 */
    void TimingWheel::InsertToWheel(int level, int slot, Node *node)
    {
        node->next           = _wheels[level][slot];
        _wheels[level][slot] = node;
    }

    /**
     * @brief O(1) 惰性删除定时器
     * @param timerID  要取消的 TimerID
     */
    void TimingWheel::Cancel(TimerID timerID)
    {
        auto it = _idMap.find(timerID);
        if (it == _idMap.end())
        {
            return;
        }

        it->second->cancelled = true;
        _idMap.erase(it);
    }

    /**
 * @brief 推进轮0 + 级联，每逻辑帧调用一次（20ms）
 */
    void TimingWheel::Tick()
    {
        // 推进轮0
        _currentSlot[0] = (_currentSlot[0] + 1) % kSlots;
        ProcessSlot(0, _currentSlot[0]);

        // 轮0 走完一圈 → 逐级向上推进
        if (_currentSlot[0] == 0)
        {
            for (int level = 1; level < kLevels; ++level)
            {
                _currentSlot[level] = (_currentSlot[level] + 1) % kSlots;
                CascadeToLower(level - 1);
                if (_currentSlot[level] != 0)
                    break;
            }
        }

        // 定期健康检查
        _tickCount++;
        if (_tickCount % (kSlots * 60) == 0) // 每 ~72s
        {
            size_t count = ActiveCount();
            if (count > kWarnThreshold)
            {
                Log::Error("TimingWheel WARNING: active timers={} exceeds threshold={} — possible timer leak",
                           count,
                           kWarnThreshold);
            }
        }
    }

    /**
     * @brief 遍历槽位链表，触发到期 / 放回 / 惰性清理
     * @param level  层级索引
     * @param slot   槽位索引
     */
    void TimingWheel::ProcessSlot(int level, int slot)
    {
        Node *cur            = _wheels[level][slot];
        _wheels[level][slot] = nullptr;

        Node *lastKept = nullptr;
        Node *keptHead = nullptr;

        while (cur)
        {
            Node *next = cur->next;

            if (cur->cancelled)
            {
                FreeNode(cur);
            }
            else if (cur->remainingRounds > 0)
            {
                cur->remainingRounds--;
                cur->next = nullptr;

                if (!keptHead)
                {
                    keptHead = cur;
                    lastKept = cur;
                }
                else
                {
                    lastKept->next = cur;
                    lastKept       = cur;
                }
            }
            else
            {
                cur->callback();
                FreeNode(cur);
            }

            cur = next;
        }

        if (keptHead)
        {
            lastKept->next       = _wheels[level][slot];
            _wheels[level][slot] = keptHead;
        }
    }

    /**
     * @brief 从 fromLevel+1 级联到 fromLevel
     *
     * 将剩余的 rounds > 0 放回 fromLevel+1，
     * rounds == 0 均匀分布到 fromLevel 的 M 个槽。
     * @param fromLevel  源层级
     */
    void TimingWheel::CascadeToLower(int fromLevel)
    {
        int upperLevel = fromLevel + 1;

        Node *cur                                     = _wheels[upperLevel][_currentSlot[upperLevel]];
        _wheels[upperLevel][_currentSlot[upperLevel]] = nullptr;

        while (cur)
        {
            Node *next = cur->next;
            cur->next  = nullptr;

            if (cur->cancelled)
            {
                FreeNode(cur);
            }
            else if (cur->remainingRounds > 0)
            {
                cur->remainingRounds--;
                InsertToWheel(upperLevel, _currentSlot[upperLevel], cur);
            }
            else
            {
                int slot             = static_cast<int>(cur->id % kSlots);
                cur->remainingRounds = 0;
                InsertToWheel(fromLevel, slot, cur);
            }

            cur = next;
        }
    }

    /**
 * @brief 当前活跃定时器数量
 */
    size_t TimingWheel::ActiveCount() const
    {
        return _idMap.size();
    }

} // namespace MMO
