#pragma once

#include "../common/types.h"

#include <vector>

namespace NGraphCompressor {

// The vertices that can still take an edge, together with how many edges each
// of them still owes.
//
// The adjacency lists are coded by splitting the vertex range in half over and
// over, and every split needs the two halves compared: how many vertices each
// holds and how many edges they owe.  A segment tree answers both by simply
// reading a node, which is what makes the split cheap; a prefix structure
// would have to add up a logarithmic number of pieces for every one of them.
class TWeightTree {
public:
    explicit TWeightTree(const std::vector<ui32>& weights)
        : Leaves_(1)
    {
        while (Leaves_ < weights.size()) {
            Leaves_ <<= 1;
        }
        Count_.assign(2 * Leaves_, 0);
        Weight_.assign(2 * Leaves_, 0);
        for (size_t i = 0; i < weights.size(); ++i) {
            Count_[Leaves_ + i] = 1;
            Weight_[Leaves_ + i] = weights[i];
        }
        for (size_t node = Leaves_ - 1; node > 0; --node) {
            Count_[node] = Count_[2 * node] + Count_[2 * node + 1];
            Weight_[node] = Weight_[2 * node] + Weight_[2 * node + 1];
        }
    }

    // The number of leaves, which is the vertex range the root covers.
    size_t GetSpan() const noexcept {
        return Leaves_;
    }

    ui32 GetCount(size_t node) const noexcept {
        return Count_[node];
    }

    ui64 GetWeight(size_t node) const noexcept {
        return Weight_[node];
    }

    void Set(size_t index, ui32 count, ui32 weight) noexcept {
        size_t node = Leaves_ + index;
        Count_[node] = count;
        Weight_[node] = weight;
        for (node >>= 1; node > 0; node >>= 1) {
            Count_[node] = Count_[2 * node] + Count_[2 * node + 1];
            Weight_[node] = Weight_[2 * node] + Weight_[2 * node + 1];
        }
    }

    // Appends every vertex of a subtree that can still take an edge.
    void Collect(size_t node, size_t lo, size_t hi, std::vector<ui32>& out) const {
        if (Count_[node] == 0) {
            return;
        }
        if (hi - lo == 1) {
            out.push_back(static_cast<ui32>(lo));
            return;
        }
        const size_t mid = (lo + hi) / 2;
        Collect(2 * node, lo, mid, out);
        Collect(2 * node + 1, mid, hi, out);
    }

private:
    size_t Leaves_;
    std::vector<ui32> Count_;
    std::vector<ui64> Weight_;
};

} // namespace NGraphCompressor
