/**
 * @file ErrorCode.cpp
 * @brief EErrorCode::ToString() 实现
 */

#include "Common/Core/ErrorCode.h"

const char *ToString(EErrorCode code)
{
    switch (code)
    {
        case EErrorCode::OK:
            return "OK";
        case EErrorCode::UNKNOWN:
            return "UNKNOWN";
        case EErrorCode::INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case EErrorCode::OUT_OF_RANGE:
            return "OUT_OF_RANGE";
        case EErrorCode::NOT_FOUND:
            return "NOT_FOUND";
        case EErrorCode::NOT_SUPPORTED:
            return "NOT_SUPPORTED";
        case EErrorCode::INTERNAL:
            return "INTERNAL";
        case EErrorCode::TIMEOUT:
            return "TIMEOUT";
        case EErrorCode::ALREADY_EXISTS:
            return "ALREADY_EXISTS";
        case EErrorCode::PERMISSION_DENIED:
            return "PERMISSION_DENIED";
        case EErrorCode::NOT_INITIALIZED:
            return "NOT_INITIALIZED";
        case EErrorCode::RESOURCE_EXHAUSTED:
            return "RESOURCE_EXHAUSTED";
    }
    return "UNKNOWN";
}
