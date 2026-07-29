#pragma once

#include "daScript/simulate/simulate.h"
#include "Common/Core/Types.h"

namespace MMO
{

    class IDasLangtHost
    {
    public:
        virtual ~IDasLangtHost() = default;

        virtual das::Context *GetScriptContext() const = 0;

        virtual das::SimFunction *GetDispatchFunc() const = 0;

        virtual void SendRawToClient(uint32 sessionID, uint32 msgID, const uint8 *data, size_t len) = 0;
    };
} // namespace MMO