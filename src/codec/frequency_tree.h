#pragma once

#include "range_coder.h"

#include "../common/types.h"

#include <vector>

namespace NGraphCompressor {

// An adaptive distribution over a range of values, kept as counts.
//
// A value is coded by walking a tree of counts from the root to its leaf, one
// binary decision per level, each with the probability the counts say.  The
// rate converges to the entropy of the values that actually occur, without
// the distribution ever being transmitted: the decoder counts the same values
// in the same order.
//
// The alternative, splitting a value into an exponent and a mantissa and
// modelling those, cannot do as well here.  It spends its accuracy on the
// magnitude and leaves the shape inside a binary bucket almost flat, while
// the degrees of a graph are neither flat inside a bucket nor smooth across
// bucket boundaries.
class TFrequencyTree {
public:
    explicit TFrequencyTree(size_t size)
        : Leaves_(1)
        , Size_(size)
    {
        while (Leaves_ < size) {
            Leaves_ <<= 1;
        }
        Count_.assign(2 * Leaves_, 0);
        for (size_t i = 0; i < size; ++i) {
            Count_[Leaves_ + i] = 1;
        }
        for (size_t node = Leaves_ - 1; node > 0; --node) {
            Count_[node] = Count_[2 * node] + Count_[2 * node + 1];
        }
    }

    void Encode(TRangeEncoder& encoder, size_t value) {
        size_t node = 1;
        size_t lo = 0;
        size_t hi = Leaves_;
        while (hi - lo > 1) {
            const size_t mid = (lo + hi) / 2;
            const ui32 leftCount = Count_[2 * node];
            const ui32 rightCount = Count_[2 * node + 1];
            const bool left = value < mid;
            if (leftCount != 0 && rightCount != 0) {
                encoder.EncodeBitWithProbability(MakeProbability(leftCount, rightCount),
                                                 left ? 0 : 1);
            }
            if (left) {
                node = 2 * node;
                hi = mid;
            } else {
                node = 2 * node + 1;
                lo = mid;
            }
        }
        Add(value);
    }

    size_t Decode(TRangeDecoder& decoder) {
        size_t node = 1;
        size_t lo = 0;
        size_t hi = Leaves_;
        while (hi - lo > 1) {
            const size_t mid = (lo + hi) / 2;
            const ui32 leftCount = Count_[2 * node];
            const ui32 rightCount = Count_[2 * node + 1];
            bool left;
            if (leftCount == 0) {
                left = false;
            } else if (rightCount == 0) {
                left = true;
            } else {
                left = decoder.DecodeBitWithProbability(MakeProbability(leftCount, rightCount)) == 0;
            }
            if (left) {
                node = 2 * node;
                hi = mid;
            } else {
                node = 2 * node + 1;
                lo = mid;
            }
        }
        Add(lo);
        return lo;
    }

private:
    // A value weighs this much more than the flat count every value starts
    // with, so the flat start fades quickly without ever reaching zero.
    static const ui32 INCREMENT = 64;

    static ui32 MakeProbability(ui32 leftCount, ui32 rightCount) noexcept {
        const ui64 total = static_cast<ui64>(leftCount) + rightCount;
        ui64 probability = (static_cast<ui64>(leftCount) << NRangeCoder::PROBABILITY_BITS) / total;
        if (probability < 1) {
            probability = 1;
        }
        if (probability > NRangeCoder::PROBABILITY_TOTAL - 1) {
            probability = NRangeCoder::PROBABILITY_TOTAL - 1;
        }
        return static_cast<ui32>(probability);
    }

    void Add(size_t value) noexcept {
        size_t node = Leaves_ + value;
        while (node > 0) {
            Count_[node] += INCREMENT;
            node >>= 1;
        }
    }

private:
    size_t Leaves_;
    size_t Size_;
    std::vector<ui32> Count_;
};

} // namespace NGraphCompressor
