/**
 * @file ScriptDispatchRegistry.cpp
 * @brief ScriptDispatchRegistry 实现 — 静态单例（兼容 AutoGen 生成器）
 */
#include "World/ScriptDispatchRegistry.h"

#include "Common/Log/Log.h"

namespace MMO
{

    std::array<ScriptDispatchFn, kMaxHandlers> &ScriptDispatchRegistry::Table()
    {
        static std::array<ScriptDispatchFn, kMaxHandlers> table {};
        return table;
    }

    void ScriptDispatchRegistry::Register(uint32 msgID, ScriptDispatchFn fn)
    {
        if (msgID >= kMaxHandlers)
        {
            Log::Error("ScriptDispatchRegistry: msgID {} exceeds kMaxHandlers", msgID);
            return;
        }
        Table()[msgID] = fn;
    }

    bool ScriptDispatchRegistry::Dispatch(WorldServer &server,
                                          uint32       sessionID,
                                          uint32       msgID,
                                          const uint8 *body,
                                          size_t       len)
    {
        if (msgID >= kMaxHandlers || !Table()[msgID])
        {
            return false;
        }
        return Table()[msgID](server, sessionID, body, len);
    }

} // namespace MMO
