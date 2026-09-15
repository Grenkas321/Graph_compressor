#pragma once

#include "graph.h"

#include <string>

namespace NGraphCompressor {

// Writes the graph back in the text format accepted by TTsvReader.  The lines
// are produced in the order of the internal representation, which is allowed
// to differ from the order of the original file.
class TTsvWriter {
public:
    void Write(const TGraph& graph, const std::string& path) const;
};

} // namespace NGraphCompressor
