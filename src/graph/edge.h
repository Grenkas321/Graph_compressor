#pragma once

#include "../common/types.h"

namespace NGraphCompressor {

// One line of the input file: an undirected weighted edge.
struct TEdge {
    ui32 First;
    ui32 Second;
    ui8 Weight;
};

} // namespace NGraphCompressor
