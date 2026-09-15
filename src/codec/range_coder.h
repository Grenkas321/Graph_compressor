#pragma once

#include "../io/byte_input.h"
#include "../io/byte_output.h"
#include "../common/types.h"

namespace NGraphCompressor {

// The state of one adaptive binary model.
//
// The low PROBABILITY_BITS bits hold the estimated chance of the next bit
// being zero, the higher bits hold a saturating counter of the observations
// made so far.  After every coded bit the estimate is moved towards the
// observed value by a fraction of 2^-shift, where the shift is taken from the
// counter: a young model follows the data quickly, a mature one keeps a low
// variance estimate.  This matters because the format mixes contexts seen
// millions of times with contexts seen a handful of times.
using TBitState = ui32;

namespace NRangeCoder {
    const size_t PROBABILITY_BITS = 16;
    const ui32 PROBABILITY_TOTAL = 1u << PROBABILITY_BITS;
    const ui32 PROBABILITY_MASK = PROBABILITY_TOTAL - 1;
    const TBitState INITIAL_STATE = PROBABILITY_TOTAL / 2;
    const size_t COUNTER_SHIFT = PROBABILITY_BITS;
    const ui32 COUNTER_LIMIT = 255;
    const ui32 TOP_VALUE = 1u << 24;
    const size_t FLUSH_BYTE_COUNT = 5;

    // The adaptation shift as a function of the observation counter: the
    // estimate moves by 2^-3 of the remaining distance while the model is
    // young and by 2^-8 once it has seen a hundred of bits.
    inline ui32 GetAdaptationShift(ui32 counter) noexcept {
        return 3 + static_cast<ui32>(counter >= 8) + static_cast<ui32>(counter >= 16)
                 + static_cast<ui32>(counter >= 32) + static_cast<ui32>(counter >= 64)
                 + static_cast<ui32>(counter >= 128);
    }
} // namespace NRangeCoder

// Binary arithmetic (range) encoder.
//
// The implementation follows the classic carry-propagating scheme used in
// LZMA: the interval is kept in a 32 bit range and a 64 bit lower bound, and
// the bytes that can no longer change are emitted as soon as possible.
class TRangeEncoder {
public:
    explicit TRangeEncoder(TByteOutput& output) noexcept;
    TRangeEncoder(const TRangeEncoder&) = delete;
    TRangeEncoder& operator=(const TRangeEncoder&) = delete;

    // Encodes a single bit using (and updating) an adaptive model.
    void EncodeBit(TBitState& state, ui32 bit) {
        ui32 probability = state & NRangeCoder::PROBABILITY_MASK;
        ui32 counter = state >> NRangeCoder::COUNTER_SHIFT;
        const ui32 shift = NRangeCoder::GetAdaptationShift(counter);
        const ui32 bound = (Range_ >> NRangeCoder::PROBABILITY_BITS) * probability;
        if (bit == 0) {
            Range_ = bound;
            probability += (NRangeCoder::PROBABILITY_TOTAL - probability) >> shift;
        } else {
            Low_ += bound;
            Range_ -= bound;
            probability -= probability >> shift;
        }
        counter += counter < NRangeCoder::COUNTER_LIMIT ? 1 : 0;
        state = (counter << NRangeCoder::COUNTER_SHIFT) | probability;
        while (Range_ < NRangeCoder::TOP_VALUE) {
            Range_ <<= 8;
            ShiftLow();
        }
    }

    // Encodes bitCount low bits of the value assuming a uniform distribution.
    void EncodeDirectBits(ui32 value, size_t bitCount) {
        for (size_t i = bitCount; i-- > 0;) {
            Range_ >>= 1;
            if (((value >> i) & 1) != 0) {
                Low_ += Range_;
            }
            while (Range_ < NRangeCoder::TOP_VALUE) {
                Range_ <<= 8;
                ShiftLow();
            }
        }
    }

    // Writes out the tail of the interval.  No bits may be encoded afterwards.
    void Finish();

private:
    void ShiftLow();

private:
    TByteOutput& Output_;
    ui64 Low_;
    ui32 Range_;
    ui8 Cache_;
    ui64 CacheSize_;
};

// Binary arithmetic (range) decoder, the exact counterpart of TRangeEncoder.
class TRangeDecoder {
public:
    explicit TRangeDecoder(TByteInput& input);
    TRangeDecoder(const TRangeDecoder&) = delete;
    TRangeDecoder& operator=(const TRangeDecoder&) = delete;

    ui32 DecodeBit(TBitState& state) {
        ui32 probability = state & NRangeCoder::PROBABILITY_MASK;
        ui32 counter = state >> NRangeCoder::COUNTER_SHIFT;
        const ui32 shift = NRangeCoder::GetAdaptationShift(counter);
        const ui32 bound = (Range_ >> NRangeCoder::PROBABILITY_BITS) * probability;
        ui32 bit;
        if (Code_ < bound) {
            Range_ = bound;
            probability += (NRangeCoder::PROBABILITY_TOTAL - probability) >> shift;
            bit = 0;
        } else {
            Code_ -= bound;
            Range_ -= bound;
            probability -= probability >> shift;
            bit = 1;
        }
        counter += counter < NRangeCoder::COUNTER_LIMIT ? 1 : 0;
        state = (counter << NRangeCoder::COUNTER_SHIFT) | probability;
        Normalize();
        return bit;
    }

    ui32 DecodeDirectBits(size_t bitCount) {
        ui32 result = 0;
        for (size_t i = 0; i < bitCount; ++i) {
            Range_ >>= 1;
            Code_ -= Range_;
            const ui32 mask = 0u - (Code_ >> 31);
            Code_ += Range_ & mask;
            result = (result << 1) + (mask + 1);
            Normalize();
        }
        return result;
    }

private:
    void Normalize() {
        while (Range_ < NRangeCoder::TOP_VALUE) {
            Code_ = (Code_ << 8) | Input_.ReadByteOrZero();
            Range_ <<= 8;
        }
    }

private:
    TByteInput& Input_;
    ui32 Range_;
    ui32 Code_;
};

} // namespace NGraphCompressor
