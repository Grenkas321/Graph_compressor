#include "serializer.h"

#include "../codec/bit_tree.h"
#include "../codec/integer_model.h"
#include "../codec/range_coder.h"
#include "../common/bit_utils.h"
#include "../common/exception.h"
#include "../common/types.h"

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

    // The adjacency gaps are conditioned on two values known to both sides:
    // the expected gap length, quantized to SUB_BUCKET_COUNT steps per binary
    // octave, and the number of neighbours that are still to be coded.  The
    // first selects the scale of the distribution, the second its shape --
    // with many neighbours left the gaps are noticeably less dispersed than a
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
} // namespace

void TGraphSerializer::Serialize(const TGraph& graph, TByteOutput& output) const {
    output.Write(FORMAT_SIGNATURE, SIGNATURE_LENGTH);

    const std::vector<ui32>& vertexIds = graph.GetVertexIds();
    const std::vector<ui64>& offsets = graph.GetAdjacencyOffsets();
    const std::vector<ui32>& targets = graph.GetAdjacencyTargets();
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

    // 3. The adjacency lists.  The length of the list of a vertex is not
    //    stored: it equals the number of its edges that have not been seen
    //    yet, which the decoder derives from the degrees.
    std::vector<ui32> remaining = degrees;
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        const ui32 listLength = remaining[vertex];
        ui64 position = vertex;
        for (ui32 i = 0; i < listLength; ++i) {
            const ui32 left = listLength - i;
            const ui64 expectedGap = (vertexCount - position) / left;
            const ui32 target = targets[offsets[vertex] + i];
            models.Gap.Encode(encoder, MakeGapContext(expectedGap, left), target - position);
            const ui8 weight = weights[offsets[vertex] + i];
            if (weightCoding == WEIGHT_CODING_ADAPTIVE) {
                EncodeWithBitTree(encoder, models.Weight.data(), WEIGHT_BITS, weight);
            } else {
                encoder.EncodeDirectBits(weight, WEIGHT_BITS);
            }
            position = static_cast<ui64>(target) + 1;
            if (target != vertex) {
                --remaining[target];
            }
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

    std::vector<ui32> remaining(vertexCount);
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        remaining[vertex] = static_cast<ui32>(models.Degree.Decode(decoder, 0) + 1);
    }

    std::vector<ui64> offsets(vertexCount + 1, 0);
    std::vector<ui32> targets;
    std::vector<ui8> weights;
    targets.reserve(edgeCount);
    weights.reserve(edgeCount);

    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        const ui32 listLength = remaining[vertex];
        ui64 position = vertex;
        for (ui32 i = 0; i < listLength; ++i) {
            Y_ENSURE(position < vertexCount, "the compressed graph is damaged: an adjacency list overflows");
            const ui32 left = listLength - i;
            const ui64 expectedGap = (vertexCount - position) / left;
            const ui64 target = position + models.Gap.Decode(decoder, MakeGapContext(expectedGap, left));
            Y_ENSURE(target < vertexCount, "the compressed graph is damaged: a vertex index is out of range");
            targets.push_back(static_cast<ui32>(target));
            weights.push_back(static_cast<ui8>(weightCoding == WEIGHT_CODING_ADAPTIVE
                                                   ? DecodeWithBitTree(decoder, models.Weight.data(), WEIGHT_BITS)
                                                   : decoder.DecodeDirectBits(WEIGHT_BITS)));
            position = target + 1;
            if (target != vertex) {
                --remaining[target];
            }
        }
        offsets[vertex + 1] = targets.size();
    }
    Y_ENSURE(targets.size() == edgeCount, "the compressed graph is damaged: the edge count does not match");

    return TGraph(std::move(vertexIds), std::move(offsets), std::move(targets), std::move(weights));
}

} // namespace NGraphCompressor
