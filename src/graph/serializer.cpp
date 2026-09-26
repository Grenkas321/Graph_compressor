#include "serializer.h"

#include "vertex_set.h"

#include "../codec/bit_tree.h"
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

    // The adjacency steps are conditioned on two values known to both sides:
    // the expected step length, quantized to SUB_BUCKET_COUNT steps per binary
    // octave, and the number of neighbours that are still to be coded.  The
    // first selects the scale of the distribution, the second its shape --
    // with many neighbours left the steps are noticeably less dispersed than a
    // geometric law would suggest.
    const size_t SUB_BUCKET_BITS = 2;
    const size_t SUB_BUCKET_COUNT = static_cast<size_t>(1) << SUB_BUCKET_BITS;
    const size_t LEFT_BUCKET_COUNT = 4;
    const size_t GAP_CONTEXT_COUNT = 33 * SUB_BUCKET_COUNT * LEFT_BUCKET_COUNT;

    size_t MakeGapContext(ui64 expectedGap, ui32 left) noexcept {
        const size_t exponent = GetBitLength(expectedGap) - 1;
        size_t fraction = 0;
        if (exponent >= SUB_BUCKET_BITS) {
            fraction = static_cast<size_t>((expectedGap >> (exponent - SUB_BUCKET_BITS)) & (SUB_BUCKET_COUNT - 1));
        }
        size_t leftBucket = GetBitLength(left) - 1;
        if (leftBucket >= LEFT_BUCKET_COUNT) {
            leftBucket = LEFT_BUCKET_COUNT - 1;
        }
        return (exponent * SUB_BUCKET_COUNT + fraction) * LEFT_BUCKET_COUNT + leftBucket;
    }

    // Holds every adaptive model used by the format.  Both directions create
    // it in the same state, which is what keeps them synchronized.
    struct TModels {
        TModels()
            : VertexId(1)
            , Degree(1)
            , Gap(GAP_CONTEXT_COUNT)
            , Weight(WEIGHT_SLOTS, NRangeCoder::INITIAL_STATE)
        {
        }

        TIntegerModel VertexId;
        TIntegerModel Degree;
        TIntegerModel Gap;
        std::vector<TBitState> Weight;
    };

    // The order in which the adjacency lists are coded: falling degree, ties
    // broken by the vertex index.
    //
    // The rank model treats every vertex that can still take an edge as
    // equally likely, but an edge lands on a vertex with a chance that grows
    // with the number of edges that vertex still owes, and the degrees follow
    // a power law.  Coding the lists in this order turns that weight into a
    // position: the vertices an edge is likely to reach are gathered at the
    // front, the ranks become small, and the adaptive model learns the shape
    // on its own.
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
    // low byte of the target so that one sort orders a list.
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

    // 1. The sorted identifiers, as the gaps between the neighbouring ones.
    ui64 previousId = 0;
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        const ui64 id = vertexIds[vertex];
        models.VertexId.Encode(encoder, 0, vertex == 0 ? id : id - previousId - 1);
        previousId = id;
    }

    // 2. The degrees.  They are at least one because isolated vertices do not
    //    occur in the input format.
    const std::vector<ui32>& degrees = graph.GetDegrees();
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        models.Degree.Encode(encoder, 0, degrees[vertex] - 1);
    }

    // 3. The adjacency lists, in the order of falling degree.  The length of
    //    the list of a vertex is not stored: it equals the number of its edges
    //    that have not been seen yet, which the decoder derives from the
    //    degrees.  A target is coded by its rank among the vertices that can
    //    still take an edge, so the vertices whose edges are all accounted for
    //    cost nothing.
    const std::vector<ui32> order = MakeCodingOrder(degrees);
    std::vector<ui32> codingIndex(vertexCount);
    std::vector<ui32> remaining(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        codingIndex[order[i]] = static_cast<ui32>(i);
        remaining[i] = degrees[order[i]];
    }
    const TCodingView view = MakeCodingView(graph, codingIndex);

    TVertexSet usable(vertexCount);
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        const ui32 listLength = remaining[vertex];
        ui64 passed = usable.CountBelow(vertex);
        for (ui32 i = 0; i < listLength; ++i) {
            const ui32 left = listLength - i;
            const ui64 reachable = usable.GetMemberCount() - passed;
            const ui64 packed = view.Targets[view.Offsets[vertex] + i];
            const ui32 target = static_cast<ui32>(packed >> 8);
            const ui64 below = usable.CountBelow(target);
            models.Gap.Encode(encoder, MakeGapContext(reachable / left, left), below - passed);
            const ui8 weight = static_cast<ui8>(packed & 0xFF);
            if (weightCoding == WEIGHT_CODING_ADAPTIVE) {
                EncodeWithBitTree(encoder, models.Weight.data(), WEIGHT_BITS, weight);
            } else {
                encoder.EncodeDirectBits(weight, WEIGHT_BITS);
            }
            if (target != vertex && --remaining[target] == 0) {
                usable.Remove(target);
            }
            // The next target is above the current one, so the members that
            // have been passed are those below it plus the target itself if
            // it is still able to take an edge.
            passed = below + (usable.Contains(target) ? 1 : 0);
        }
        // The lists of the vertices that follow can no longer reach this one.
        if (usable.Contains(vertex)) {
            usable.Remove(vertex);
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

    std::vector<ui32> vertexIds(vertexCount);
    ui64 previousId = 0;
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        const ui64 gap = models.VertexId.Decode(decoder, 0);
        const ui64 id = vertex == 0 ? gap : previousId + gap + 1;
        Y_ENSURE(id <= 0xFFFFFFFFull, "the compressed graph is damaged: a vertex identifier overflows");
        vertexIds[vertex] = static_cast<ui32>(id);
        previousId = id;
    }

    std::vector<ui32> degrees(vertexCount);
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        degrees[vertex] = static_cast<ui32>(models.Degree.Decode(decoder, 0) + 1);
    }

    // The same permutation the encoder used, rebuilt from the degrees alone.
    const std::vector<ui32> order = MakeCodingOrder(degrees);
    std::vector<ui32> remaining(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        remaining[i] = degrees[order[i]];
    }

    // The edges arrive in the coding order and are collected as pairs of
    // identifier indices, which is what the graph is built from.
    std::vector<ui32> sources(edgeCount);
    std::vector<ui64> packedTargets(edgeCount);
    ui64 decoded = 0;

    TVertexSet usable(vertexCount);
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        const ui32 listLength = remaining[vertex];
        ui64 passed = usable.CountBelow(vertex);
        for (ui32 i = 0; i < listLength; ++i) {
            const ui32 left = listLength - i;
            const ui64 reachable = usable.GetMemberCount() - passed;
            Y_ENSURE(reachable >= left, "the compressed graph is damaged: an adjacency list overflows");
            const ui64 rank = models.Gap.Decode(decoder, MakeGapContext(reachable / left, left));
            Y_ENSURE(rank + left <= reachable, "the compressed graph is damaged: a vertex rank is out of range");
            const ui64 below = passed + rank;
            const size_t target = usable.Select(below);
            const ui8 weight = static_cast<ui8>(weightCoding == WEIGHT_CODING_ADAPTIVE
                                                    ? DecodeWithBitTree(decoder, models.Weight.data(), WEIGHT_BITS)
                                                    : decoder.DecodeDirectBits(WEIGHT_BITS));

            Y_ENSURE(decoded < edgeCount, "the compressed graph is damaged: too many edges");
            const ui32 first = order[vertex];
            const ui32 second = order[target];
            sources[decoded] = first < second ? first : second;
            packedTargets[decoded] = (static_cast<ui64>(first < second ? second : first) << 8) | weight;
            ++decoded;

            if (target != vertex && --remaining[target] == 0) {
                usable.Remove(target);
            }
            passed = below + (usable.Contains(target) ? 1 : 0);
        }
        if (usable.Contains(vertex)) {
            usable.Remove(vertex);
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
