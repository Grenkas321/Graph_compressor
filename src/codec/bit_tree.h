#pragma once

#include "range_coder.h"

#include "../common/types.h"

namespace NGraphCompressor {

// Codes a bitCount wide value with a binary tree of adaptive probabilities.
// The tree learns the whole distribution of the value, so the achieved rate
// converges to its entropy while only bitCount coder calls are made.
// The probabilities array must contain 2^bitCount elements; the element with
// index zero is not used.
inline void EncodeWithBitTree(TRangeEncoder& encoder, TBitState* probabilities, size_t bitCount, ui32 value) {
    size_t node = 1;
    for (size_t i = bitCount; i-- > 0;) {
        const ui32 bit = (value >> i) & 1;
        encoder.EncodeBit(probabilities[node], bit);
        node = (node << 1) | bit;
    }
}

inline ui32 DecodeWithBitTree(TRangeDecoder& decoder, TBitState* probabilities, size_t bitCount) {
    size_t node = 1;
    for (size_t i = 0; i < bitCount; ++i) {
        node = (node << 1) | decoder.DecodeBit(probabilities[node]);
    }
    return static_cast<ui32>(node - (static_cast<size_t>(1) << bitCount));
}

} // namespace NGraphCompressor
