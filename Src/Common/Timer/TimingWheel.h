/**
 * @file TimingWheel.h
 * @brief 三级时间轮定时器
 *
 * 轮0: 50ms×60=3s    轮1: 3s×60=3min    轮2: 3min×60=3h
 * O(1) Schedule / Cancel（惰性删除）/ Tick
 * 单线程专用——仅在逻辑线程调用
 */
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <unordered_map>
#include <vector>

#include "Common/Core/Types.h"

namespace MMO
{

/**
 * @brief 三级时间轮定时器
 *
 * 基于分级时间轮算法，支持 O(1) 插入/取消，惰性删除。
 * 最大延迟 3h，超出则 clamp 到 3h。
 */
class TimingWheel
{
public:
    using TimerID = uint64;                    ///< 定时器唯一 ID
    using Callback = std::function<void()>;   ///< 定时器回调

    TimingWheel();
    ~TimingWheel();

    TimingWheel(const TimingWheel&) = delete;
    TimingWheel& operator=(const TimingWheel&) = delete;

    /**
     * @brief 调度一个定时器
     * @param delay  延迟时间（上限 3h，超出则 clamp）
     * @param cb     到期回调
     * @return 唯一 TimerID
     */
    TimerID Schedule(std::chrono::milliseconds delay, Callback cb);

    /**
     * @brief O(1) 惰性删除定时器
     * @param timerID  要取消的 TimerID
     */
    void Cancel(TimerID timerID);

    /** @brief 每逻辑帧调用一次（50ms），推进时间轮并触发到期回调 */
    void Tick();

    /** @brief 当前活跃定时器数量 */
    size_t ActiveCount() const;

    static constexpr std::chrono::milliseconds kMaxDelay{3 * 60 * 60 * 1000}; ///< 最大延迟 3h
    static constexpr int kSlots = 60;   ///< 每层槽位数
    static constexpr int kLevels = 3;   ///< 层级数

    /** @brief 活跃定时器告警阈值（可能泄漏） */
    static constexpr size_t kWarnThreshold = 10000;

private:
    /** @brief 槽位链表节点 */
    struct Node
    {
        TimerID id = 0;
        int remainingRounds = 0;
        Callback callback;
        Node* next = nullptr;
        bool cancelled = false;
    };

    Node* AllocNode(TimerID id, int rounds, Callback cb);
    void FreeNode(Node* node);
    void InsertToWheel(int level, int slot, Node* node);

    static constexpr std::chrono::milliseconds kTickSizes[kLevels] = {
        std::chrono::milliseconds(50),     ///< 轮0: 50ms
        std::chrono::milliseconds(3000),   ///< 轮1: 3s
        std::chrono::milliseconds(180000)  ///< 轮2: 3min
    };

    static TimerID GenTimerID();
    void ClearFreeList();
    void ProcessSlot(int level, int slot);
    void CascadeToLower(int fromLevel);

    Node* _wheels[kLevels][kSlots] = {};
    std::unordered_map<TimerID, Node*> _idMap;
    int _currentSlot[kLevels] = {};
    Node* _freeList = nullptr;
    int _tickCount = 0;

    static std::atomic<TimerID> _nextTimerID;
};

} // namespace MMO
