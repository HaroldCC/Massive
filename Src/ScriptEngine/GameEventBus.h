#pragma once
#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 游戏事件负载（序列化后的 protobuf 字节，值语义）
     *
     * data 指向 GameEventBus 内部稳定存储（帧内有效，派发后清空）。
     * ⚠ data 是【帧作用域】指针——仅在 Drain() 返回后的同一帧内有效：
     * 下一帧 Emit() 会从缓冲头覆盖。脚本侧 handler 必须【同步消费】payload
     * （宏内部 reinterpret 成类型化事件做值拷贝），禁止把 data 存进全局/跨帧保留。
     * 这是零 per-event 堆分配（铁律 2）与安全性的折中。
     */
    struct GameEventEnvelope
    {
        uint16       event_type = 0;       // EGameEventType（type 是 das 保留字，故命名 event_type）
        uint32       size       = 0;       // payload 字节数
        const uint8 *data       = nullptr; // payload（bus 内，只读，帧内有效）
    };

    /**
     * @brief 每场景一个事件总线（C++ 写，脚本读）
     *
     * payload 存固定容量缓冲——零 per-event 堆分配（铁律 2 纪律）。
     * 数据只读（const uint8*），脚本不得写总线内部缓冲。
     */
    class GameEventBus
    {
    public:
        /** @brief 构造：预分配 payload 缓冲（64KB，铁律 2 预算） */
        GameEventBus() : _payload(kMaxPayloadBytes)
        {
        }

        template <typename TEvent>
        void Emit(uint16 event_type, const TEvent &ev)
        {
            if (_envelopes.size() >= kMaxEventsPerFrame)
            {
                return; // 事件预算
            }
            const size_t sz = static_cast<size_t>(ev.ByteSizeLong());
            if (_payloadUsed + sz > _payload.size())
            {
                return;
            }
            uint8                      *dst = _payload.data() + _payloadUsed;
            [[maybe_unused]] const bool ok  = ev.SerializeToArray(dst, static_cast<int>(sz));
            if (!ok)
            {
                return; // 序列化失败：跳过（预算内不应发生）
            }
            _envelopes.push_back({event_type, static_cast<uint32>(sz), dst});
            _payloadUsed += sz;
        }

        /**
         * @brief 取走本帧全部事件（交换缓冲，重置 payload 指针区）。
         * @return 事件列表——data 指针指向本帧缓冲，须同步消费后丢弃。
         */
        std::vector<GameEventEnvelope> Drain()
        {
            std::vector<GameEventEnvelope> result;
            result.swap(_envelopes);
            _payloadUsed = 0;
            return result;
        }

        size_t Count() const
        {
            return _envelopes.size();
        }

        static constexpr size_t kMaxEventsPerFrame = 1024; // 铁律 2
        static constexpr size_t kMaxPayloadBytes   = 64 * 1024;

    private:
        std::vector<GameEventEnvelope> _envelopes;
        std::vector<uint8>             _payload; // 容量在 ctor 预分配
        size_t                         _payloadUsed = 0;
    };

} // namespace MMO