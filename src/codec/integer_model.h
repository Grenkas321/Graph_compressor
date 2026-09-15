#pragma once

#include "range_coder.h"

#include "../common/types.h"

#include <vector>

namespace NGraphCompressor {

// Adaptive model of non-negative integers.
//
// A value is coded as the pair (exponent, mantissa) of the binary
// representation of value + 1:
//
//     value + 1 = 2^exponent + mantissa,   0 <= mantissa < 2^exponent
//
// The exponent is coded with a binary tree, therefore its whole distribution
// is learned and the rate converges to the exponent entropy.  The highest
// MANTISSA_BITS bits of the mantissa are coded with a second tree selected by
// the exponent, which captures the slope of the distribution inside a binary
// bucket; the remaining bits are almost uniform and are written directly.
//
// For geometrically distributed values this scheme stays within a few
// thousandths of a bit from the source entropy at any scale, which is why the
// same model can be reused for vertex identifiers, degrees and adjacency gaps.
//
// Several independent statistics ("contexts") are kept in a single instance so
// that the caller can condition the model on the expected magnitude of the
// value without allocating a model per context.
class TIntegerModel {
public:
    explicit TIntegerModel(size_t contextCount);

    void Encode(TRangeEncoder& encoder, size_t context, ui64 value);
    ui64 Decode(TRangeDecoder& decoder, size_t context);

private:
    static const size_t EXPONENT_BITS = 6;
    static const size_t EXPONENT_SLOTS = static_cast<size_t>(1) << EXPONENT_BITS;
    static const size_t MANTISSA_BITS = 4;
    static const size_t MANTISSA_SLOTS = static_cast<size_t>(1) << MANTISSA_BITS;

    TBitState* GetExponentRow(size_t context) noexcept {
        return Exponent_.data() + context * EXPONENT_SLOTS;
    }

    TBitState* GetMantissaRow(size_t context, size_t exponent) noexcept {
        return Mantissa_.data() + (context * EXPONENT_SLOTS + exponent) * MANTISSA_SLOTS;
    }

private:
    size_t ContextCount_;
    std::vector<TBitState> Exponent_;
    std::vector<TBitState> Mantissa_;
};

} // namespace NGraphCompressor
