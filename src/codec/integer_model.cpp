#include "integer_model.h"

#include "bit_tree.h"

#include "../common/bit_utils.h"
#include "../common/exception.h"

namespace NGraphCompressor {

namespace {
    // The direct bit primitives of the range coder operate on 32 bit words,
    // so wider tails are written in two steps.
    void EncodeWideDirectBits(TRangeEncoder& encoder, ui64 value, size_t bitCount) {
        if (bitCount > 32) {
            encoder.EncodeDirectBits(static_cast<ui32>(value >> 32), bitCount - 32);
            bitCount = 32;
        }
        encoder.EncodeDirectBits(static_cast<ui32>(value), bitCount);
    }

    ui64 DecodeWideDirectBits(TRangeDecoder& decoder, size_t bitCount) {
        ui64 result = 0;
        if (bitCount > 32) {
            result = static_cast<ui64>(decoder.DecodeDirectBits(bitCount - 32)) << 32;
            bitCount = 32;
        }
        return result | decoder.DecodeDirectBits(bitCount);
    }
} // namespace

TIntegerModel::TIntegerModel(size_t contextCount)
    : ContextCount_(contextCount)
    , Exponent_(contextCount * EXPONENT_SLOTS, NRangeCoder::INITIAL_STATE)
    , Mantissa_(contextCount * EXPONENT_SLOTS * MANTISSA_SLOTS, NRangeCoder::INITIAL_STATE)
{
    Y_ENSURE(contextCount > 0, "an integer model needs at least one context");
}

void TIntegerModel::Encode(TRangeEncoder& encoder, size_t context, ui64 value) {
    Y_ENSURE(context < ContextCount_, "integer model context " << context << " is out of range");
    Y_ENSURE(value != ~static_cast<ui64>(0), "value is too large for the integer model");

    const ui64 shifted = value + 1;
    const size_t exponent = GetBitLength(shifted) - 1;
    EncodeWithBitTree(encoder, GetExponentRow(context), EXPONENT_BITS, static_cast<ui32>(exponent));
    if (exponent == 0) {
        return;
    }

    const ui64 mantissa = shifted - (static_cast<ui64>(1) << exponent);
    const size_t modelledBits = exponent < MANTISSA_BITS ? exponent : MANTISSA_BITS;
    const size_t directBits = exponent - modelledBits;
    EncodeWithBitTree(encoder, GetMantissaRow(context, exponent), modelledBits,
                      static_cast<ui32>(mantissa >> directBits));
    if (directBits != 0) {
        EncodeWideDirectBits(encoder, mantissa & ((static_cast<ui64>(1) << directBits) - 1), directBits);
    }
}

ui64 TIntegerModel::Decode(TRangeDecoder& decoder, size_t context) {
    Y_ENSURE(context < ContextCount_, "integer model context " << context << " is out of range");

    const size_t exponent = DecodeWithBitTree(decoder, GetExponentRow(context), EXPONENT_BITS);
    if (exponent == 0) {
        return 0;
    }

    const size_t modelledBits = exponent < MANTISSA_BITS ? exponent : MANTISSA_BITS;
    const size_t directBits = exponent - modelledBits;
    ui64 mantissa = DecodeWithBitTree(decoder, GetMantissaRow(context, exponent), modelledBits);
    if (directBits != 0) {
        mantissa = (mantissa << directBits) | DecodeWideDirectBits(decoder, directBits);
    }
    return ((static_cast<ui64>(1) << exponent) | mantissa) - 1;
}

} // namespace NGraphCompressor
