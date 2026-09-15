#pragma once

#include "graph.h"

#include "../io/byte_input.h"
#include "../io/byte_output.h"

namespace NGraphCompressor {

// Writes a graph as a compressed byte stream.
//
// The layout of the stream is described in README.md; in short it consists of
// the signature, the sorted vertex identifiers, the vertex degrees and the
// adjacency lists with the edge weights.  Everything except the signature goes
// through an adaptive binary range coder.
class TGraphSerializer {
public:
    void Serialize(const TGraph& graph, TByteOutput& output) const;
};

// Restores a graph written by TGraphSerializer.
class TGraphDeserializer {
public:
    TGraph Deserialize(TByteInput& input) const;
};

} // namespace NGraphCompressor
