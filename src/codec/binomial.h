#pragma once

#include "range_coder.h"

#include "../common/types.h"

#include <vector>

namespace NGraphCompressor {

// The distribution of how many of several items fall on the left of a split
// whose probability of going left is known.
//
// The outcome is coded by halving the range of possible counts, each step a
// single binary decision whose probability is the share of the distribution
// that lies in the lower half.  A symbol coder over quantized frequencies
// would be one call instead of a few, but it divides the coder range by the
// total and throws the remainder away, and it has to reserve a unit of that
// total for every outcome the split could have had; on a graph of this size
// both add up to more than the extra calls cost.
//
// Both directions build the table from the same arguments with the same code,
// so they walk the same halvings.
class TBinomial {
public:
    // Prepares the distribution of the count for a split of the given width
    // whose probability of going left is p, in units of 1 / 2^16.
    void Build(size_t width, ui32 probability) {
        Cumulative_.assign(width + 2, 0.0);
        Weights_.assign(width + 1, 0.0);

        const double p = static_cast<double>(probability) / NRangeCoder::PROBABILITY_TOTAL;
        const double ratio = p / (1.0 - p);

        // The weights grow outwards from the mode: starting anywhere else
        // lets the first terms underflow when the width is large.
        size_t mode = static_cast<size_t>(static_cast<double>(width + 1) * p);
        if (mode > width) {
            mode = width;
        }
        Weights_[mode] = 1.0;
        double value = 1.0;
        for (size_t i = mode; i > 0; --i) {
            value *= static_cast<double>(i) / (static_cast<double>(width - i + 1) * ratio);
            if (value < 1e-300) {
                break;
            }
            Weights_[i - 1] = value;
        }
        value = 1.0;
        for (size_t i = mode; i < width; ++i) {
            value *= static_cast<double>(width - i) / static_cast<double>(i + 1) * ratio;
            if (value < 1e-300) {
                break;
            }
            Weights_[i + 1] = value;
        }

        // Every outcome keeps a floor so that the halvings never divide by
        // zero and no count becomes impossible to code.
        for (size_t i = 0; i <= width; ++i) {
            Cumulative_[i + 1] = Cumulative_[i] + Weights_[i] + FLOOR;
        }
        Width_ = width;
    }

    void Encode(TRangeEncoder& encoder, size_t count) const {
        size_t lo = 0;
        size_t hi = Width_;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            const ui32 probability = LowerProbability(lo, mid, hi);
            const ui32 bit = count <= mid ? 0 : 1;
            encoder.EncodeBitWithProbability(probability, bit);
            if (bit == 0) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
    }

    size_t Decode(TRangeDecoder& decoder) const {
        size_t lo = 0;
        size_t hi = Width_;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            const ui32 probability = LowerProbability(lo, mid, hi);
            if (decoder.DecodeBitWithProbability(probability) == 0) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        return lo;
    }

private:
    // A floor small enough not to disturb the distribution and large enough
    // to keep every outcome reachable.
    static constexpr double FLOOR = 1e-12;

    // The chance that the count is at most mid, given that it lies in
    // [lo, hi].
    ui32 LowerProbability(size_t lo, size_t mid, size_t hi) const noexcept {
        const double lower = Cumulative_[mid + 1] - Cumulative_[lo];
        const double whole = Cumulative_[hi + 1] - Cumulative_[lo];
        double share = lower / whole;
        if (!(share > 0.0)) {
            share = 0.0;
        }
        if (share > 1.0) {
            share = 1.0;
        }
        ui32 probability = static_cast<ui32>(share * NRangeCoder::PROBABILITY_TOTAL);
        if (probability < 1) {
            probability = 1;
        }
        if (probability > NRangeCoder::PROBABILITY_TOTAL - 1) {
            probability = NRangeCoder::PROBABILITY_TOTAL - 1;
        }
        return probability;
    }

private:
    size_t Width_ = 0;
    std::vector<double> Weights_;
    std::vector<double> Cumulative_;
};

} // namespace NGraphCompressor
