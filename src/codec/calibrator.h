#pragma once

#include "range_coder.h"

#include "../common/types.h"

#include <cmath>
#include <vector>

namespace NGraphCompressor {

// Corrects a probability the format computes from the graph itself.
//
// The chance that a split sends an item to the left is taken from the edges
// the two halves still owe.  That is a model of the graph, not the graph, and
// wherever it is systematically off the same error repeats over millions of
// splits.  The correction is additive in the logistic domain and is indexed
// by the computed probability, so a probability the model already gets right
// keeps a correction of zero and is left alone.
//
// The correction is applied by scaling the weight of the left half, which
// keeps the arithmetic exact when the correction is zero, and the scale comes
// from a table, so no logarithm is evaluated per split.  The step of the
// update decays as a bucket fills: the estimate has to end up finer than the
// error it is correcting, which is what a fixed step could never deliver.
class TCalibrator {
public:
    TCalibrator(size_t contextCount)
        : ContextCount_(contextCount)
        , Delta_(contextCount * BUCKET_COUNT, 0)
        , Seen_(contextCount * BUCKET_COUNT, 0)
    {
        BuildTables();
    }

    size_t GetContextCount() const noexcept {
        return ContextCount_;
    }

    // The log odds of a probability, scaled by 512.  Callers use it to build
    // the context, where only the rough magnitude matters.
    static int GetStretch(ui32 probability) noexcept {
        BuildTables();
        return Stretch(probability);
    }

    // The bucket of a probability, with the position between two buckets in
    // the low FRACTION_BITS bits.
    static ui32 GetPosition(ui32 probability) noexcept {
        int stretched = Stretch(probability);
        if (stretched < -RANGE) {
            stretched = -RANGE;
        }
        if (stretched > RANGE) {
            stretched = RANGE;
        }
        const i64 scaled = static_cast<i64>(stretched + RANGE) * (BUCKET_COUNT - 1);
        i64 position = (scaled << FRACTION_BITS) / (2 * RANGE);
        const i64 limit = static_cast<i64>(BUCKET_COUNT - 2) << FRACTION_BITS;
        if (position > limit) {
            position = limit;
        }
        if (position < 0) {
            position = 0;
        }
        return static_cast<ui32>(position);
    }

    // The corrected probability of going left, given the weights of the two
    // halves.  Scaling a weight rather than the probability keeps the answer
    // exactly equal to the input when the correction is zero.
    ui32 Apply(size_t context, ui32 position, ui64 leftWeight, ui64 rightWeight) const noexcept {
        const size_t low = position >> FRACTION_BITS;
        const ui32 fraction = position & ((1u << FRACTION_BITS) - 1);
        const size_t base = context * BUCKET_COUNT;
        const i64 correction =
            (static_cast<i64>(Delta_[base + low]) * ((1 << FRACTION_BITS) - fraction)
             + static_cast<i64>(Delta_[base + low + 1]) * fraction) >> FRACTION_BITS;

        const ui64 scale = Exponent(correction);
        const ui64 left = leftWeight * scale;
        const ui64 right = rightWeight << SCALE_BITS;
        ui64 probability = (left << NRangeCoder::PROBABILITY_BITS) / (left + right);
        if (probability < 1) {
            probability = 1;
        }
        if (probability > NRangeCoder::PROBABILITY_TOTAL - 1) {
            probability = NRangeCoder::PROBABILITY_TOTAL - 1;
        }
        return static_cast<ui32>(probability);
    }

    // Moves the correction towards what actually happened.  The observation
    // is the share of the items that went left, in units of 1 / 2^16.
    void Update(size_t context, ui32 position, ui32 used, ui32 observed, ui32 weight) noexcept {
        const size_t low = position >> FRACTION_BITS;
        const ui32 fraction = position & ((1u << FRACTION_BITS) - 1);
        const size_t base = context * BUCKET_COUNT;
        const i64 gradient = static_cast<i64>(observed) - static_cast<i64>(used);

        AdjustBucket(base + low, gradient, (1 << FRACTION_BITS) - fraction, weight);
        AdjustBucket(base + low + 1, gradient, fraction, weight);
    }

private:
    // The correction is held in units of 1 / 2^28 of a natural log odds.
    //
    // The precision is not about how fine the correction has to be, it is
    // about the step never rounding to nothing.  A bucket where one outcome
    // is rare sees a small nudge on every common outcome and a large one on
    // every rare outcome; if the small nudge truncates away and the large one
    // does not, the correction walks off in the direction of the rare
    // outcome and never comes back.
    static const int DELTA_BITS = 28;
    static const i32 DELTA_LIMIT = 4 << DELTA_BITS;
    static const size_t BUCKET_COUNT = 129;
    static const int RANGE = 13 * 512;          // the stretch scale is 512
    static const int FRACTION_BITS = 8;
    static const int SCALE_BITS = 16;
    static const int STRETCH_SLOTS = 1 << NRangeCoder::PROBABILITY_BITS;
    static const int EXPONENT_SLOTS = 8193;
    static const i64 EXPONENT_STEP = (static_cast<i64>(8) << DELTA_BITS) / (EXPONENT_SLOTS - 1);

    // The step of the update: a fifth of the way towards what happened while
    // a bucket is new, falling off as the bucket fills.  A correction has to
    // end up finer than the error it is correcting, which a fixed step could
    // never deliver, and the numbers below were measured on the sample graphs
    // over a range wide enough that the choice is not a fit to one file.
    static const i64 LEARNING_STEP = 13107;
    static const i64 LEARNING_DECAY = 262144;

    void AdjustBucket(size_t index, i64 gradient, ui32 share, ui32 weight) noexcept {
        const i64 denominator = LEARNING_DECAY + static_cast<i64>(Seen_[index]);
        const i64 step = (LEARNING_STEP * static_cast<i64>(share) * gradient) / denominator;
        i64 value = static_cast<i64>(Delta_[index]) + step;
        if (value > DELTA_LIMIT) {
            value = DELTA_LIMIT;
        }
        if (value < -DELTA_LIMIT) {
            value = -DELTA_LIMIT;
        }
        Delta_[index] = static_cast<i32>(value);
        const ui64 seen = Seen_[index] + static_cast<ui64>(weight) * share / (1 << FRACTION_BITS);
        Seen_[index] = seen < (1ull << 40) ? seen : (1ull << 40);
    }

    // The tables are built once, from the same code in both directions.
    static void BuildTables() {
        if (!StretchTable().empty()) {
            return;
        }
        std::vector<i16>& stretch = StretchTable();
        stretch.resize(STRETCH_SLOTS);
        for (int i = 0; i < STRETCH_SLOTS; ++i) {
            const double p = (static_cast<double>(i) + 0.5) / STRETCH_SLOTS;
            double value = 512.0 * std::log(p / (1.0 - p));
            if (value > 32767.0) {
                value = 32767.0;
            }
            if (value < -32767.0) {
                value = -32767.0;
            }
            stretch[i] = static_cast<i16>(value > 0 ? value + 0.5 : value - 0.5);
        }

        std::vector<ui32>& exponent = ExponentTable();
        exponent.resize(EXPONENT_SLOTS);
        for (int i = 0; i < EXPONENT_SLOTS; ++i) {
            // The correction spans [-4, 4] natural log odds over the table,
            // with the middle slot landing exactly on zero so that a bucket
            // that has learned nothing leaves the probability untouched.
            const double x = -4.0 + 8.0 * static_cast<double>(i) / (EXPONENT_SLOTS - 1);
            exponent[i] = static_cast<ui32>(std::exp(x) * (1 << SCALE_BITS) + 0.5);
        }
    }

    static std::vector<i16>& StretchTable() {
        static std::vector<i16> table;
        return table;
    }

    static std::vector<ui32>& ExponentTable() {
        static std::vector<ui32> table;
        return table;
    }

    static int Stretch(ui32 probability) noexcept {
        const std::vector<i16>& table = StretchTable();
        size_t index = probability;
        if (index >= static_cast<size_t>(STRETCH_SLOTS)) {
            index = STRETCH_SLOTS - 1;
        }
        return table[index];
    }

    static ui64 Exponent(i64 correction) noexcept {
        const std::vector<ui32>& table = ExponentTable();
        i64 scaled = (correction + (static_cast<i64>(4) << DELTA_BITS)) / EXPONENT_STEP;
        if (scaled < 0) {
            scaled = 0;
        }
        if (scaled >= EXPONENT_SLOTS) {
            scaled = EXPONENT_SLOTS - 1;
        }
        return table[static_cast<size_t>(scaled)];
    }

private:
    size_t ContextCount_;
    std::vector<i32> Delta_;
    std::vector<ui64> Seen_;
};

} // namespace NGraphCompressor
