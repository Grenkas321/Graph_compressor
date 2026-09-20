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

// Returns the number of bits set in a value.
inline size_t GetPopCount(ui64 value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<size_t>(__builtin_popcountll(value));
#else
    size_t count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
#endif
}

// Returns the position of the lowest bit set in a value, which must not be
// zero.  GetLowestBitIndex(1) == 0.
inline size_t GetLowestBitIndex(ui64 value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<size_t>(__builtin_ctzll(value));
#else
    size_t index = 0;
    while ((value & 1) == 0) {
        value >>= 1;
        ++index;
    }
    return index;
#endif
}

} // namespace NGraphCompressor
