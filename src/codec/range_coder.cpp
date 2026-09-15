#include "range_coder.h"

namespace NGraphCompressor {



TRangeEncoder::TRangeEncoder(TByteOutput& output) noexcept
    : Output_(output)
    , Low_(0)
    , Range_(0xFFFFFFFFu)
    , Cache_(0)
    , CacheSize_(1)
{
}

void TRangeEncoder::Finish() {
    for (size_t i = 0; i < NRangeCoder::FLUSH_BYTE_COUNT; ++i) {
        ShiftLow();
    }
}

void TRangeEncoder::ShiftLow() {
    if (static_cast<ui32>(Low_) < 0xFF000000u || (Low_ >> 32) != 0) {
        ui8 carry = static_cast<ui8>(Low_ >> 32);
        ui8 current = Cache_;
        do {
            Output_.WriteByte(static_cast<ui8>(current + carry));
            current = 0xFF;
        } while (--CacheSize_ != 0);
        Cache_ = static_cast<ui8>(Low_ >> 24);
    }
    ++CacheSize_;
    Low_ = static_cast<ui64>(static_cast<ui32>(Low_) & 0x00FFFFFFu) << 8;
}

TRangeDecoder::TRangeDecoder(TByteInput& input)
    : Input_(input)
    , Range_(0xFFFFFFFFu)
    , Code_(0)
{
    // The encoder always emits one leading byte that carries no information.
    for (size_t i = 0; i < NRangeCoder::FLUSH_BYTE_COUNT; ++i) {
        Code_ = (Code_ << 8) | Input_.ReadByteOrZero();
    }
}

} // namespace NGraphCompressor
