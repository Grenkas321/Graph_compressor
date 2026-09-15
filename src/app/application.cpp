#include "application.h"

#include "../graph/graph.h"
#include "../graph/serializer.h"
#include "../graph/tsv_reader.h"
#include "../graph/tsv_writer.h"
#include "../io/byte_input.h"
#include "../io/byte_output.h"

#include <vector>

namespace NGraphCompressor {

void TApplication::Run() const {
    if (Options_.GetMode() == MODE_SERIALIZE) {
        Serialize();
    } else {
        Deserialize();
    }
}

void TApplication::Serialize() const {
    std::vector<TEdge> edges = TTsvReader().Read(Options_.GetInputPath());
    const TGraph graph = TGraph::FromEdges(edges);

    TByteOutput output(Options_.GetOutputPath());
    TGraphSerializer().Serialize(graph, output);
}

void TApplication::Deserialize() const {
    const std::vector<ui8> content = ReadWholeFile(Options_.GetInputPath());
    TByteInput input(content.data(), content.size());
    const TGraph graph = TGraphDeserializer().Deserialize(input);

    TTsvWriter().Write(graph, Options_.GetOutputPath());
}

} // namespace NGraphCompressor
