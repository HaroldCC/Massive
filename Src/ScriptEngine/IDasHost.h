#pragma once

#include "daScript/simulate/simulate.h"
#include "Common/Core/Types.h"

namespace MMO
{

    class IDasLangHost
    {
    public:
        virtual ~IDasLangHost() = default;

        virtual void SendRawToClient(uint32 sessionID, uint32 msgID, const uint8 *data, size_t len) = 0;
    };
} // namespace MMO