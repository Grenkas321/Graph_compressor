#pragma once

#include "edge.h"

#include "../common/types.h"

#include <string>
#include <vector>

namespace NGraphCompressor {

// Reads the text representation of a graph: one edge per line, three decimal
// numbers separated by tab characters.
class TTsvReader {
public:
    std::vector<TEdge> Read(const std::string& path) const;
};

} // namespace NGraphCompressor
