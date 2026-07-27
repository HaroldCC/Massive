#pragma once

#include <vector>
#include <cstdint>
#include "daScript/simulate/simulate.h"

namespace DasHelpers {

template <typename T>
inline das::TArray<T> CreateDasArrayFromVector(das::Context * ctx, const std::vector<T> &v) {
    das::TArray<T> arr; // default constructed empty
    if (v.empty()) return arr;

    // allocate raw buffer using context's allocator
    // Context::allocate returns char* suitable for placement-new
    char * raw = nullptr;
    try {
        raw = ctx->allocate(sizeof(T) * v.size(), nullptr);
    } catch (...) {
        // allocation failed
        return arr;
    }
    if (!raw) return arr;

    T * elems = reinterpret_cast<T*>(raw);
    for (size_t i = 0; i < v.size(); ++i) {
        new (&elems[i]) T(v[i]); // placement new
    }

    // set TArray internals - existing code uses .data and .size
    // data is stored as a char* in many daslib headers; cast elems back to char*
    arr.data = reinterpret_cast<char*>(elems);
    arr.size = static_cast<uint32_t>(v.size());
#if defined(__cpp_if_constexpr) || 1
    // try to set capacity if present (many das versions have capacity)
    // Use pointer-to-member offset trick is unsafe; we'll attempt a best-effort by
    // writing to arr.capacity if it exists at compile time. If not present, this
    // will be a compile error — but project compiles with current daScript which
    // defines size/capacity as used in existing code. So we set it.
    // Note: if capacity member doesn't exist, remove this line.
    arr.capacity = static_cast<uint32_t>(v.size());
#endif
    return arr;
}

} // namespace DasHelpers
