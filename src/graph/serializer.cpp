#include "serializer.h"

#include "weight_tree.h"

#include "../codec/binomial.h"
#include "../codec/bit_tree.h"
#include "../codec/calibrator.h"
#include "../codec/frequency_tree.h"
#include "../codec/integer_model.h"
#include "../codec/range_coder.h"
#include "../common/bit_utils.h"
#include "../common/exception.h"
#include "../common/types.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace NGraphCompressor {

namespace {
    const ui8 FORMAT_SIGNATURE[] = {'G', 'R', 'C', '1'};
    const size_t SIGNATURE_LENGTH = sizeof(FORMAT_SIGNATURE);

    const size_t COUNTER_BITS = 32;
    const size_t WEIGHT_BITS = 8;
    const size_t WEIGHT_SLOTS = static_cast<size_t>(1) << WEIGHT_BITS;

    // The identifiers live in [0, 2^32).
    const ui64 ID_SPACE = static_cast<ui64>(1) << 32;

    // How the edge weights are coded.  The statement promises uniformly
    // distributed weights, and for such data raw bits are slightly cheaper
    // than an adaptive model, which always pays for its own learning.  The
    // decision is made from the actual histogram and stored in the stream, so
    // skewed weights are still compressed.
    enum EWeightCoding {
        WEIGHT_CODING_DIRECT = 0,
        WEIGHT_CODING_ADAPTIVE = 1,
    };

    // The measured cost of the adaptive weight model on incompressible data.
    const double WEIGHT_MODEL_OVERHEAD_BITS = 0.05;

    EWeightCoding ChooseWeightCoding(const std::vector<ui8>& weights) {
        std::vector<ui64> histogram(WEIGHT_SLOTS, 0);
        for (ui8 weight : weights) {
            ++histogram[weight];
        }

        const double total = static_cast<double>(weights.size());
        double entropy = 0.0;
        for (ui64 count : histogram) {
            if (count != 0) {
                const double probability = static_cast<double>(count) / total;
                entropy -= probability * std::log2(probability);
            }
        }
        return entropy + WEIGHT_MODEL_OVERHEAD_BITS < static_cast<double>(WEIGHT_BITS)
                   ? WEIGHT_CODING_ADAPTIVE
                   : WEIGHT_CODING_DIRECT;
    }

    // The order in which the adjacency lists are coded: falling degree, ties
    // broken by the vertex index.
    //
    // An edge lands on a vertex with a chance that grows with the number of
    // edges that vertex still owes, and the degrees follow a power law.  This
    // order puts the vertices an edge is likely to reach at the front, which
    // is what makes the halves of a split differ enough to be worth coding.
    //
    // The order costs nothing to transmit.  The identifiers and the degrees
    // are coded in the order of the identifiers, and the degrees precede the
    // adjacency lists, so the decoder rebuilds the very same permutation.
    // A counting sort keeps it linear and gives both sides the same answer.
    std::vector<ui32> MakeCodingOrder(const std::vector<ui32>& degrees) {
        const size_t vertexCount = degrees.size();
        ui32 maxDegree = 0;
        for (ui32 degree : degrees) {
            if (degree > maxDegree) {
                maxDegree = degree;
            }
        }

        std::vector<ui32> start(static_cast<size_t>(maxDegree) + 2, 0);
        for (ui32 degree : degrees) {
            ++start[maxDegree - degree + 1];
        }
        for (size_t i = 1; i < start.size(); ++i) {
            start[i] += start[i - 1];
        }

        std::vector<ui32> order(vertexCount);
        for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
            order[start[maxDegree - degrees[vertex]]++] = static_cast<ui32>(vertex);
        }
        return order;
    }

    // The adjacency lists in the coding order.  Every edge sits at the
    // endpoint that comes first in that order, and the weight travels in the
    // low byte of the target so that one sort orders a list and a comparison
    // against a shifted bound still compares targets.
    struct TCodingView {
        std::vector<ui64> Offsets;
        std::vector<ui64> Targets;
    };

    TCodingView MakeCodingView(const TGraph& graph, const std::vector<ui32>& codingIndex) {
        const size_t vertexCount = graph.GetVertexCount();
        const std::vector<ui64>& offsets = graph.GetAdjacencyOffsets();
        const std::vector<ui32>& targets = graph.GetAdjacencyTargets();
        const std::vector<ui8>& weights = graph.GetEdgeWeights();

        TCodingView view;
        view.Offsets.assign(vertexCount + 1, 0);
        for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
            const ui32 first = codingIndex[vertex];
            for (ui64 i = offsets[vertex]; i < offsets[vertex + 1]; ++i) {
                const ui32 second = codingIndex[targets[i]];
                ++view.Offsets[(first < second ? first : second) + 1];
            }
        }
        for (size_t i = 1; i <= vertexCount; ++i) {
            view.Offsets[i] += view.Offsets[i - 1];
        }

        view.Targets.resize(graph.GetEdgeCount());
        std::vector<ui64> cursors(view.Offsets.begin(), view.Offsets.end() - 1);
        for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
            const ui32 first = codingIndex[vertex];
            for (ui64 i = offsets[vertex]; i < offsets[vertex + 1]; ++i) {
                const ui32 second = codingIndex[targets[i]];
                const ui32 low = first < second ? first : second;
                const ui32 high = first < second ? second : first;
                view.Targets[cursors[low]++] = (static_cast<ui64>(high) << 8) | weights[i];
            }
        }

        for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
            std::sort(view.Targets.begin() + view.Offsets[vertex],
                      view.Targets.begin() + view.Offsets[vertex + 1]);
        }
        return view;
    }

    // The context a split is calibrated in.  A split is described by how many
    // vertices it has to choose from, by how far the weights move it away
    // from plain counting, and by how many targets it distributes.
    const size_t SIZE_CLASSES = 8;
    const size_t DISCREPANCY_CLASSES = 9;
    const size_t WIDTH_CLASSES = 4;
    const size_t SPAN_CLASSES = 8;
    const size_t CALIBRATION_CONTEXTS =
        SIZE_CLASSES * DISCREPANCY_CLASSES * WIDTH_CLASSES * SPAN_CLASSES;

    size_t MakeWidthClass(size_t width) noexcept {
        if (width <= 1) {
            return 0;
        }
        if (width == 2) {
            return 1;
        }
        return width <= 4 ? 2 : 3;
    }

    size_t MakeSplitContext(ui32 available, ui32 weightProbability, ui32 countProbability,
                            size_t width, size_t span) noexcept {
        size_t size = GetBitLength(available);
        if (size >= SIZE_CLASSES) {
            size = SIZE_CLASSES - 1;
        }

        // How much of the range is still alive: in this order the free
        // vertices are far from evenly spread, so the length of the range and
        // the number of vertices in it say different things.
        size_t spanClass = GetBitLength(span) - GetBitLength(available);
        if (spanClass >= SPAN_CLASSES) {
            spanClass = SPAN_CLASSES - 1;
        }

        // The stretch scale is 512, and the classes span two natural log odds
        // in each direction.
        int difference = TCalibrator::GetStretch(weightProbability)
                       - TCalibrator::GetStretch(countProbability);
        if (difference < -1024) {
            difference = -1024;
        }
        if (difference > 1023) {
            difference = 1023;
        }
        const size_t discrepancy = static_cast<size_t>((difference + 1024) / 256);

        return ((size * DISCREPANCY_CLASSES + discrepancy) * WIDTH_CLASSES + MakeWidthClass(width))
                   * SPAN_CLASSES + spanClass;
    }

    ui32 MakeProbability(ui64 left, ui64 right) noexcept {
        ui64 probability = (left << NRangeCoder::PROBABILITY_BITS) / (left + right);
        if (probability < 1) {
            probability = 1;
        }
        if (probability > NRangeCoder::PROBABILITY_TOTAL - 1) {
            probability = NRangeCoder::PROBABILITY_TOTAL - 1;
        }
        return static_cast<ui32>(probability);
    }

    // Holds every adaptive model used by the format.  Both directions create
    // it in the same state, which is what keeps them synchronized.
    struct TModels {
        TModels()
            : Degree(1)
            , Calibration(CALIBRATION_CONTEXTS)
            , Weight(WEIGHT_SLOTS, NRangeCoder::INITIAL_STATE)
        {
        }

        TIntegerModel Degree;
        TCalibrator Calibration;
        TBinomial Binomial;
        std::vector<TBitState> Weight;
    };

    // Everything a split needs, computed the same way by both directions.
    struct TSplit {
        size_t Context;
        ui32 Position;
        ui32 Probability;
    };

    TSplit PrepareSplit(TModels& models, const TWeightTree& tree, size_t node,
                        ui32 available, size_t width, size_t span) {
        const size_t leftNode = 2 * node;
        const size_t rightNode = leftNode + 1;
        const ui64 leftWeight = tree.GetWeight(leftNode);
        const ui64 rightWeight = tree.GetWeight(rightNode);
        const ui32 weightProbability = MakeProbability(leftWeight, rightWeight);
        const ui32 countProbability = MakeProbability(tree.GetCount(leftNode),
                                                      tree.GetCount(rightNode));

        TSplit split;
        split.Context = MakeSplitContext(available, weightProbability, countProbability,
                                         width, span);
        split.Position = TCalibrator::GetPosition(weightProbability);
        split.Probability = models.Calibration.Apply(split.Context, split.Position,
                                                     leftWeight, rightWeight);
        return split;
    }

    void LearnSplit(TModels& models, const TSplit& split, size_t width, size_t left) {
        const ui32 observed = static_cast<ui32>(
            (static_cast<ui64>(left) << NRangeCoder::PROBABILITY_BITS) / width);
        models.Calibration.Update(split.Context, split.Position, split.Probability,
                                  observed, static_cast<ui32>(width));
    }

    // Codes the identifiers by splitting the whole identifier space in half
    // over and over, the same way an adjacency list is coded.
    //
    // The statement promises the identifiers are uniform, so the halves are
    // equally likely and no model is needed at all: the count that falls on
    // the left is binomial with probability one half.  The cost then comes
    // out at log2 C(2^32, N), the information there actually is in a set of N
    // identifiers, which an adaptive model of the gaps only approaches.
    //
    // Nothing is coded where the answer is forced: an empty side, a side with
    // no identifiers, or a range every value of which is an identifier.
    void EncodeIdentifiers(TRangeEncoder& encoder, TBinomial& binomial,
                           ui64 lo, ui64 hi, const ui32* first, const ui32* last) {
        const size_t width = static_cast<size_t>(last - first);
        if (width == 0 || width == hi - lo || hi - lo == 1) {
            return;
        }
        const ui64 mid = lo + (hi - lo) / 2;
        const ui32* split = std::lower_bound(first, last, static_cast<ui32>(mid));
        const size_t left = static_cast<size_t>(split - first);
        binomial.Build(width, NRangeCoder::PROBABILITY_TOTAL / 2,
                       static_cast<ui32>(mid - lo), static_cast<ui32>(hi - mid));
        binomial.Encode(encoder, left);
        EncodeIdentifiers(encoder, binomial, lo, mid, first, split);
        EncodeIdentifiers(encoder, binomial, mid, hi, split, last);
    }

    void DecodeIdentifiers(TRangeDecoder& decoder, TBinomial& binomial,
                           ui64 lo, ui64 hi, size_t width, std::vector<ui32>& out) {
        if (width == 0) {
            return;
        }
        if (width == hi - lo) {
            for (ui64 value = lo; value < hi; ++value) {
                out.push_back(static_cast<ui32>(value));
            }
            return;
        }
        if (hi - lo == 1) {
            out.push_back(static_cast<ui32>(lo));
            return;
        }
        const ui64 mid = lo + (hi - lo) / 2;
        binomial.Build(width, NRangeCoder::PROBABILITY_TOTAL / 2,
                       static_cast<ui32>(mid - lo), static_cast<ui32>(hi - mid));
        const size_t left = binomial.Decode(decoder);
        Y_ENSURE(left <= width, "the compressed graph is damaged: an identifier split is out of range");
        DecodeIdentifiers(decoder, binomial, lo, mid, left, out);
        DecodeIdentifiers(decoder, binomial, mid, hi, width - left, out);
    }

    // Codes one adjacency list by splitting the vertex range in half over and
    // over.  Nothing is coded where the answer is forced: a half with no
    // vertex left, a half with no target, or a range where every vertex that
    // can still take an edge is a target.
    void EncodeList(TRangeEncoder& encoder, TModels& models, const TWeightTree& tree,
                    size_t node, size_t lo, size_t hi, const ui64* first, const ui64* last) {
        const size_t width = static_cast<size_t>(last - first);
        if (width == 0) {
            return;
        }
        const ui32 available = tree.GetCount(node);
        if (width == available || hi - lo == 1) {
            return;
        }

        const size_t mid = (lo + hi) / 2;
        const size_t leftNode = 2 * node;
        const size_t rightNode = leftNode + 1;
        if (tree.GetCount(leftNode) == 0) {
            EncodeList(encoder, models, tree, rightNode, mid, hi, first, last);
            return;
        }
        if (tree.GetCount(rightNode) == 0) {
            EncodeList(encoder, models, tree, leftNode, lo, mid, first, last);
            return;
        }

        const ui64* split = std::lower_bound(first, last, static_cast<ui64>(mid) << 8);
        const size_t left = static_cast<size_t>(split - first);

        const TSplit prepared = PrepareSplit(models, tree, node, available, width, hi - lo);
        if (width == 1) {
            encoder.EncodeBitWithProbability(prepared.Probability, left == 1 ? 0 : 1);
        } else {
            models.Binomial.Build(width, prepared.Probability,
                                  tree.GetCount(leftNode), tree.GetCount(rightNode));
            models.Binomial.Encode(encoder, left);
        }
        LearnSplit(models, prepared, width, left);

        EncodeList(encoder, models, tree, leftNode, lo, mid, first, split);
        EncodeList(encoder, models, tree, rightNode, mid, hi, split, last);
    }

    void DecodeList(TRangeDecoder& decoder, TModels& models, const TWeightTree& tree,
                    size_t node, size_t lo, size_t hi, size_t width, std::vector<ui32>& out) {
        if (width == 0) {
            return;
        }
        const ui32 available = tree.GetCount(node);
        Y_ENSURE(width <= available, "the compressed graph is damaged: an adjacency list overflows");
        if (width == available) {
            tree.Collect(node, lo, hi, out);
            return;
        }
        if (hi - lo == 1) {
            out.push_back(static_cast<ui32>(lo));
            return;
        }

        const size_t mid = (lo + hi) / 2;
        const size_t leftNode = 2 * node;
        const size_t rightNode = leftNode + 1;
        if (tree.GetCount(leftNode) == 0) {
            DecodeList(decoder, models, tree, rightNode, mid, hi, width, out);
            return;
        }
        if (tree.GetCount(rightNode) == 0) {
            DecodeList(decoder, models, tree, leftNode, lo, mid, width, out);
            return;
        }

        const TSplit prepared = PrepareSplit(models, tree, node, available, width, hi - lo);
        size_t left;
        if (width == 1) {
            left = decoder.DecodeBitWithProbability(prepared.Probability) == 0 ? 1 : 0;
        } else {
            models.Binomial.Build(width, prepared.Probability,
                                  tree.GetCount(leftNode), tree.GetCount(rightNode));
            left = models.Binomial.Decode(decoder);
        }
        Y_ENSURE(left <= width, "the compressed graph is damaged: a split is out of range");
        LearnSplit(models, prepared, width, left);

        DecodeList(decoder, models, tree, leftNode, lo, mid, left, out);
        DecodeList(decoder, models, tree, rightNode, mid, hi, width - left, out);
    }
} // namespace

void TGraphSerializer::Serialize(const TGraph& graph, TByteOutput& output) const {
    output.Write(FORMAT_SIGNATURE, SIGNATURE_LENGTH);

    const std::vector<ui32>& vertexIds = graph.GetVertexIds();
    const std::vector<ui8>& weights = graph.GetEdgeWeights();
    const size_t vertexCount = graph.GetVertexCount();
    const ui64 edgeCount = graph.GetEdgeCount();

    TRangeEncoder encoder(output);
    TModels models;

    encoder.EncodeDirectBits(static_cast<ui32>(vertexCount), COUNTER_BITS);
    encoder.EncodeDirectBits(static_cast<ui32>(edgeCount >> COUNTER_BITS), COUNTER_BITS);
    encoder.EncodeDirectBits(static_cast<ui32>(edgeCount), COUNTER_BITS);

    const EWeightCoding weightCoding = ChooseWeightCoding(weights);
    encoder.EncodeDirectBits(static_cast<ui32>(weightCoding), 1);

    // 1. The sorted identifiers.
    EncodeIdentifiers(encoder, models.Binomial, 0, ID_SPACE,
                      vertexIds.data(), vertexIds.data() + vertexCount);

    // 2. The degrees.  They are at least one because isolated vertices do not
    //    occur in the input format.  The largest of them opens the range the
    //    adaptive distribution is kept over.
    const std::vector<ui32>& degrees = graph.GetDegrees();
    ui32 maxDegree = 1;
    for (ui32 degree : degrees) {
        if (degree > maxDegree) {
            maxDegree = degree;
        }
    }
    models.Degree.Encode(encoder, 0, maxDegree - 1);

    TFrequencyTree degreeModel(maxDegree);
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        degreeModel.Encode(encoder, degrees[vertex] - 1);
    }

    // 3. The adjacency lists, in the order of falling degree.  The length of
    //    a list is not stored: it equals the number of edges of that vertex
    //    that have not been seen yet, which the decoder derives from the
    //    degrees.
    const std::vector<ui32> order = MakeCodingOrder(degrees);
    std::vector<ui32> codingIndex(vertexCount);
    std::vector<ui32> remaining(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        codingIndex[order[i]] = static_cast<ui32>(i);
        remaining[i] = degrees[order[i]];
    }
    const TCodingView view = MakeCodingView(graph, codingIndex);

    TWeightTree tree(remaining);
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        const ui64 begin = view.Offsets[vertex];
        const ui64 end = view.Offsets[vertex + 1];
        if (begin != end) {
            EncodeList(encoder, models, tree, 1, 0, tree.GetSpan(),
                       view.Targets.data() + begin, view.Targets.data() + end);
        }

        // The weights follow the list, in the order of the targets, which
        // both sides know once the list itself is out.
        for (ui64 i = begin; i < end; ++i) {
            const ui8 weight = static_cast<ui8>(view.Targets[i] & 0xFF);
            if (weightCoding == WEIGHT_CODING_ADAPTIVE) {
                EncodeWithBitTree(encoder, models.Weight.data(), WEIGHT_BITS, weight);
            } else {
                encoder.EncodeDirectBits(weight, WEIGHT_BITS);
            }
        }

        for (ui64 i = begin; i < end; ++i) {
            const ui32 target = static_cast<ui32>(view.Targets[i] >> 8);
            if (target != vertex && --remaining[target] == 0) {
                tree.Set(target, 0, 0);
            } else if (target != vertex) {
                tree.Set(target, 1, remaining[target]);
            }
        }
        if (tree.GetCount(tree.GetSpan() + vertex) != 0) {
            tree.Set(vertex, 0, 0);
        }
    }

    encoder.Finish();
    output.Finish();
}

TGraph TGraphDeserializer::Deserialize(TByteInput& input) const {
    for (size_t i = 0; i < SIGNATURE_LENGTH; ++i) {
        Y_ENSURE(input.ReadByte() == FORMAT_SIGNATURE[i], "the input file is not a compressed graph");
    }

    TRangeDecoder decoder(input);
    TModels models;

    const size_t vertexCount = decoder.DecodeDirectBits(COUNTER_BITS);
    ui64 edgeCount = static_cast<ui64>(decoder.DecodeDirectBits(COUNTER_BITS)) << COUNTER_BITS;
    edgeCount |= decoder.DecodeDirectBits(COUNTER_BITS);
    const EWeightCoding weightCoding = static_cast<EWeightCoding>(decoder.DecodeDirectBits(1));
    Y_ENSURE(vertexCount > 0, "the compressed graph declares no vertices");

    std::vector<ui32> vertexIds;
    vertexIds.reserve(vertexCount);
    DecodeIdentifiers(decoder, models.Binomial, 0, ID_SPACE, vertexCount, vertexIds);
    Y_ENSURE(vertexIds.size() == vertexCount,
             "the compressed graph is damaged: the identifiers are incomplete");

    const ui64 maxDegree = models.Degree.Decode(decoder, 0) + 1;
    Y_ENSURE(maxDegree > 0 && maxDegree <= vertexCount,
             "the compressed graph is damaged: the largest degree is out of range");

    std::vector<ui32> degrees(vertexCount);
    TFrequencyTree degreeModel(static_cast<size_t>(maxDegree));
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        degrees[vertex] = static_cast<ui32>(degreeModel.Decode(decoder) + 1);
    }

    // The same permutation the encoder used, rebuilt from the degrees alone.
    const std::vector<ui32> order = MakeCodingOrder(degrees);
    std::vector<ui32> remaining(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        remaining[i] = degrees[order[i]];
    }

    std::vector<ui32> sources(edgeCount);
    std::vector<ui64> packedTargets(edgeCount);
    ui64 decoded = 0;

    TWeightTree tree(remaining);
    std::vector<ui32> list;
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        const ui32 listLength = remaining[vertex];
        list.clear();
        if (listLength != 0) {
            DecodeList(decoder, models, tree, 1, 0, tree.GetSpan(), listLength, list);
        }
        Y_ENSURE(list.size() == listLength, "the compressed graph is damaged: a list is incomplete");
        Y_ENSURE(decoded + listLength <= edgeCount, "the compressed graph is damaged: too many edges");

        for (size_t i = 0; i < list.size(); ++i) {
            const ui8 weight = static_cast<ui8>(weightCoding == WEIGHT_CODING_ADAPTIVE
                                                    ? DecodeWithBitTree(decoder, models.Weight.data(), WEIGHT_BITS)
                                                    : decoder.DecodeDirectBits(WEIGHT_BITS));
            const ui32 first = order[vertex];
            const ui32 second = order[list[i]];
            sources[decoded] = first < second ? first : second;
            packedTargets[decoded] = (static_cast<ui64>(first < second ? second : first) << 8) | weight;
            ++decoded;
        }

        for (size_t i = 0; i < list.size(); ++i) {
            const ui32 target = list[i];
            if (target != vertex && --remaining[target] == 0) {
                tree.Set(target, 0, 0);
            } else if (target != vertex) {
                tree.Set(target, 1, remaining[target]);
            }
        }
        if (tree.GetCount(tree.GetSpan() + vertex) != 0) {
            tree.Set(vertex, 0, 0);
        }
    }
    Y_ENSURE(decoded == edgeCount, "the compressed graph is damaged: the edge count does not match");

    // Back to the order of the identifiers, the order the graph is stored in.
    std::vector<ui64> offsets(vertexCount + 1, 0);
    for (ui64 i = 0; i < edgeCount; ++i) {
        ++offsets[sources[i] + 1];
    }
    for (size_t vertex = 1; vertex <= vertexCount; ++vertex) {
        offsets[vertex] += offsets[vertex - 1];
    }

    std::vector<ui64> adjacency(edgeCount);
    {
        std::vector<ui64> cursors(offsets.begin(), offsets.end() - 1);
        for (ui64 i = 0; i < edgeCount; ++i) {
            adjacency[cursors[sources[i]]++] = packedTargets[i];
        }
    }
    std::vector<ui32>().swap(sources);
    std::vector<ui64>().swap(packedTargets);

    std::vector<ui32> targets(edgeCount);
    std::vector<ui8> weights(edgeCount);
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        std::sort(adjacency.begin() + offsets[vertex], adjacency.begin() + offsets[vertex + 1]);
        for (ui64 i = offsets[vertex]; i < offsets[vertex + 1]; ++i) {
            targets[i] = static_cast<ui32>(adjacency[i] >> 8);
            weights[i] = static_cast<ui8>(adjacency[i] & 0xFF);
        }
    }

    return TGraph(std::move(vertexIds), std::move(offsets), std::move(targets), std::move(weights));
}

} // namespace NGraphCompressor
