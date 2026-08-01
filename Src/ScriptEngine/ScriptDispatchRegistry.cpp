/**
 * @file ScriptDispatchRegistry.cpp
 * @brief ScriptDispatchRegistry 实现 — 静态单例（兼容 AutoGen 生成器）
 */
#include "ScriptDispatchRegistry.h"

#include "Common/Log/Log.h"

namespace MMO
{
    void ScriptDispatchRegistry::Register(uint32 msgID, ScriptDispatchFn fn)
    {
        if (msgID >= kMaxHandlers)
        {
            Log::Error("ScriptDispatchRegistry: msgID {} exceeds kMaxHandlers", msgID);
            return;
        }

        _msgFuncs[msgID] = fn;
    }

    bool ScriptDispatchRegistry::Dispatch(uint32 sessionID, uint32 msgID, const uint8 *body, size_t len)
    {
        if (msgID >= kMaxHandlers)
        {
            return false;
        }
        auto func = _msgFuncs[msgID];
        if (nullptr == func)
        {
            return false;
        }

        return func(sessionID, body, len);
    }

} // namespace MMO
