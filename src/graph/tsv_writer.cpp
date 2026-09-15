#include "tsv_writer.h"

#include "../common/types.h"
#include "../io/byte_output.h"

namespace NGraphCompressor {

namespace {
    // The longest line is "4294967295\t4294967295\t255\n".
    const size_t MAX_LINE_LENGTH = 32;

    size_t FormatUnsigned(ui8* buffer, ui32 value) noexcept {
        ui8 digits[10];
        size_t count = 0;
        do {
            digits[count++] = static_cast<ui8>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        for (size_t i = 0; i < count; ++i) {
            buffer[i] = digits[count - 1 - i];
        }
        return count;
    }
} // namespace

void TTsvWriter::Write(const TGraph& graph, const std::string& path) const {
    TByteOutput output(path);

    const std::vector<ui32>& vertexIds = graph.GetVertexIds();
    const std::vector<ui64>& offsets = graph.GetAdjacencyOffsets();
    const std::vector<ui32>& targets = graph.GetAdjacencyTargets();
    const std::vector<ui8>& weights = graph.GetEdgeWeights();

    for (size_t vertex = 0; vertex < vertexIds.size(); ++vertex) {
        const ui32 sourceId = vertexIds[vertex];
        for (ui64 i = offsets[vertex]; i < offsets[vertex + 1]; ++i) {
            ui8* line = output.Reserve(MAX_LINE_LENGTH);
            size_t length = FormatUnsigned(line, sourceId);
            line[length++] = '\t';
            length += FormatUnsigned(line + length, vertexIds[targets[i]]);
            line[length++] = '\t';
            length += FormatUnsigned(line + length, weights[i]);
            line[length++] = '\n';
            output.Commit(length);
        }
    }

    output.Finish();
}

} // namespace NGraphCompressor
