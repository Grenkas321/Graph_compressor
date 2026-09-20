#pragma once

#include "../common/bit_utils.h"
#include "../common/types.h"

#include <vector>

namespace NGraphCompressor {

// The set of vertices that can still take an edge.
//
// A vertex all of whose edges have already been coded can never be a target
// again, and both the encoder and the decoder know it: the counters are
// derived from the degrees, which precede the adjacency lists in the stream.
// Addressing a target by its rank in this set, rather than by its distance in
// vertex numbers, removes the dead vertices from every value the format codes.
//
// The membership flags are kept as a bitmap, and a Fenwick tree counts the
// members of every block of BLOCK_BITS flags.  Both questions the format asks
// -- how many members precede an index, and which index holds a given rank --
// then cost a walk over a tree three orders of magnitude smaller than the
// vertex range plus a few population counts inside one block.  A Fenwick tree
// over the flags themselves answers the same questions with the same number of
// steps, but it walks megabytes instead of kilobytes, and the format asks one
// of these questions for every edge of the graph.
class TVertexSet {
public:
    explicit TVertexSet(size_t size)
        : Size_(size)
        , MemberCount_(size)
        , BlockCount_((size + BLOCK_BITS - 1) / BLOCK_BITS)
        , Step_(1)
        , Words_((size + WORD_BITS - 1) / WORD_BITS, ~static_cast<ui64>(0))
        , BlockTree_(BlockCount_ + 1, 0)
    {
        // Every vertex starts as a member; the bits above the last vertex stay
        // clear so that they are never selected.
        const size_t tail = size % WORD_BITS;
        if (tail != 0) {
            Words_.back() = (static_cast<ui64>(1) << tail) - 1;
        }

        // Building the tree in place is linear, whereas inserting the blocks
        // one by one would cost a logarithm each.
        for (size_t block = 1; block <= BlockCount_; ++block) {
            const size_t begin = (block - 1) * BLOCK_BITS;
            const size_t end = begin + BLOCK_BITS < size ? begin + BLOCK_BITS : size;
            BlockTree_[block] += static_cast<ui32>(end - begin);
            const size_t parent = block + (block & (~block + 1));
            if (parent <= BlockCount_) {
                BlockTree_[parent] += BlockTree_[block];
            }
        }
        while ((Step_ << 1) <= BlockCount_) {
            Step_ <<= 1;
        }
    }

    bool Contains(size_t index) const noexcept {
        return ((Words_[index / WORD_BITS] >> (index % WORD_BITS)) & 1) != 0;
    }

    ui64 GetMemberCount() const noexcept {
        return MemberCount_;
    }

    void Remove(size_t index) noexcept {
        Words_[index / WORD_BITS] &= ~(static_cast<ui64>(1) << (index % WORD_BITS));
        --MemberCount_;
        for (size_t i = index / BLOCK_BITS + 1; i <= BlockCount_; i += i & (~i + 1)) {
            --BlockTree_[i];
        }
    }

    // The number of members whose index is below the given one.  Removing the
    // member at that very index does not change the answer, which is what lets
    // the caller reuse the value after the removal.
    ui64 CountBelow(size_t index) const noexcept {
        ui64 count = 0;
        for (size_t block = index / BLOCK_BITS; block > 0; block -= block & (~block + 1)) {
            count += BlockTree_[block];
        }
        const size_t word = index / WORD_BITS;
        for (size_t i = (index / BLOCK_BITS) * BLOCK_WORDS; i < word; ++i) {
            count += GetPopCount(Words_[i]);
        }
        const size_t bit = index % WORD_BITS;
        if (bit != 0) {
            count += GetPopCount(Words_[word] & ((static_cast<ui64>(1) << bit) - 1));
        }
        return count;
    }

    // The index of the member with the given rank, counted from zero.  The
    // rank must be below the number of members.
    size_t Select(ui64 rank) const noexcept {
        size_t block = 0;
        ui64 rest = rank + 1;
        for (size_t step = Step_; step != 0; step >>= 1) {
            const size_t next = block + step;
            if (next <= BlockCount_ && BlockTree_[next] < rest) {
                block = next;
                rest -= BlockTree_[next];
            }
        }

        size_t word = block * BLOCK_WORDS;
        for (;;) {
            const size_t count = GetPopCount(Words_[word]);
            if (rest <= count) {
                break;
            }
            rest -= count;
            ++word;
        }
        return word * WORD_BITS + SelectInWord(Words_[word], rest - 1);
    }

private:
    static const size_t WORD_BITS = 64;
    static const size_t BLOCK_WORDS = 8;
    static const size_t BLOCK_BITS = WORD_BITS * BLOCK_WORDS;

    // The position of the bit with the given rank inside a single word.  The
    // word is scanned by bytes so that neither loop can run more than eight
    // times.
    static size_t SelectInWord(ui64 word, size_t rank) noexcept {
        for (size_t shift = 0; shift < WORD_BITS; shift += 8) {
            ui64 chunk = (word >> shift) & 0xFF;
            const size_t count = GetPopCount(chunk);
            if (rank < count) {
                while (rank != 0) {
                    chunk &= chunk - 1;
                    --rank;
                }
                return shift + GetLowestBitIndex(chunk);
            }
            rank -= count;
        }
        return 0;
    }

private:
    size_t Size_;
    ui64 MemberCount_;
    size_t BlockCount_;
    size_t Step_;
    std::vector<ui64> Words_;
    std::vector<ui32> BlockTree_;
};

} // namespace NGraphCompressor
