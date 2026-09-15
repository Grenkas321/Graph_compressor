#pragma once

#include "edge.h"

#include "../common/types.h"

#include <vector>

namespace NGraphCompressor {

// Undirected weighted graph without multiple edges, stored in a compact form.
//
// Vertices are renumbered by the ascending order of their identifiers, so the
// index of a vertex is its rank in the sorted identifier list.  Every edge is
// kept exactly once, at the endpoint with the smaller index; the resulting
// adjacency lists are strictly increasing, which is what the serializer needs.
// A loop is stored at its own vertex and contributes one to the degree.
class TGraph {
public:
    TGraph() = default;

    // Builds the graph from an unordered edge list.  The list is consumed.
    static TGraph FromEdges(std::vector<TEdge>& edges);

    // Builds the graph from an already prepared compact representation; used
    // by the deserializer, which restores the arrays directly.
    TGraph(std::vector<ui32>&& vertexIds,
           std::vector<ui64>&& adjacencyOffsets,
           std::vector<ui32>&& adjacencyTargets,
           std::vector<ui8>&& edgeWeights);

    size_t GetVertexCount() const noexcept {
        return VertexIds_.size();
    }

    ui64 GetEdgeCount() const noexcept {
        return AdjacencyTargets_.size();
    }

    const std::vector<ui32>& GetVertexIds() const noexcept {
        return VertexIds_;
    }

    // Number of edges incident to every vertex, a loop counted once.
    const std::vector<ui32>& GetDegrees() const noexcept {
        return Degrees_;
    }

    const std::vector<ui64>& GetAdjacencyOffsets() const noexcept {
        return AdjacencyOffsets_;
    }

    const std::vector<ui32>& GetAdjacencyTargets() const noexcept {
        return AdjacencyTargets_;
    }

    const std::vector<ui8>& GetEdgeWeights() const noexcept {
        return EdgeWeights_;
    }

private:
    void CalculateDegrees();

private:
    std::vector<ui32> VertexIds_;
    std::vector<ui32> Degrees_;
    std::vector<ui64> AdjacencyOffsets_;
    std::vector<ui32> AdjacencyTargets_;
    std::vector<ui8> EdgeWeights_;
};

} // namespace NGraphCompressor
