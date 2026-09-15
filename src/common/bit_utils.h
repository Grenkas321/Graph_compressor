#pragma once

#include "types.h"

namespace NGraphCompressor {

// Returns the number of significant bits of a value, i.e. the minimal n such
// that value < 2^n.  GetBitLength(0) == 0, GetBitLength(1) == 1.
inline size_t GetBitLength(ui64 value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return value == 0 ? 0 : 64 - static_cast<size_t>(__builtin_clzll(value));
#else
    size_t length = 0;
    while (value != 0) {
        ++length;
        value >>= 1;
    }
    return length;
#endif
}

} // namespace NGraphCompressor
