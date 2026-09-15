#include "graph.h"

#include "../common/exception.h"

#include <algorithm>
#include <utility>

namespace NGraphCompressor {

namespace {
    // Number of high identifier bits used by the lookup table that replaces a
    // binary search over the sorted identifiers.  Since the identifiers are
    // spread over the whole 32 bit range, a bucket holds a couple of entries
    // and the lookup becomes a short linear scan.
    const size_t LOOKUP_BITS = 20;
    const size_t LOOKUP_SIZE = static_cast<size_t>(1) << LOOKUP_BITS;
    const size_t LOOKUP_SHIFT = 32 - LOOKUP_BITS;

    // Buckets larger than this are ordered by the general purpose sort, the
    // small ones (the overwhelming majority) by insertion sort.
    const size_t INSERTION_SORT_LIMIT = 24;

    // Builds the table of the first identifier index for every bucket.
    std::vector<ui32> BuildLookupTable(const std::vector<ui32>& vertexIds) {
        std::vector<ui32> table(LOOKUP_SIZE + 1, 0);
        for (ui32 id : vertexIds) {
            ++table[(id >> LOOKUP_SHIFT) + 1];
        }
        for (size_t i = 1; i < table.size(); ++i) {
            table[i] += table[i - 1];
        }
        return table;
    }

    ui32 FindVertexIndex(const std::vector<ui32>& vertexIds, const std::vector<ui32>& lookupTable, ui32 id) {
        const size_t bucket = id >> LOOKUP_SHIFT;
        size_t index = lookupTable[bucket];
        const size_t bucketEnd = lookupTable[bucket + 1];
        while (index < bucketEnd && vertexIds[index] != id) {
            ++index;
        }
        Y_ENSURE(index < bucketEnd, "internal error: vertex " << id << " is missing from the index");
        return static_cast<ui32>(index);
    }

    void SortAdjacencyBucket(ui64* begin, ui64* end) {
        const size_t size = static_cast<size_t>(end - begin);
        if (size <= INSERTION_SORT_LIMIT) {
            for (ui64* it = begin + 1; it < end; ++it) {
                const ui64 value = *it;
                ui64* hole = it;
                while (hole > begin && hole[-1] > value) {
                    hole[0] = hole[-1];
                    --hole;
                }
                *hole = value;
            }
        } else {
            std::sort(begin, end);
        }
    }
} // namespace

TGraph::TGraph(std::vector<ui32>&& vertexIds,
               std::vector<ui64>&& adjacencyOffsets,
               std::vector<ui32>&& adjacencyTargets,
               std::vector<ui8>&& edgeWeights)
    : VertexIds_(std::move(vertexIds))
    , AdjacencyOffsets_(std::move(adjacencyOffsets))
    , AdjacencyTargets_(std::move(adjacencyTargets))
    , EdgeWeights_(std::move(edgeWeights))
{
    CalculateDegrees();
}

TGraph TGraph::FromEdges(std::vector<TEdge>& edges) {
    Y_ENSURE(!edges.empty(), "the input graph contains no edges");

    TGraph graph;

    // 1. Collect the distinct vertex identifiers in ascending order.
    {
        std::vector<ui32> ids;
        ids.reserve(edges.size() * 2);
        for (const TEdge& edge : edges) {
            ids.push_back(edge.First);
            ids.push_back(edge.Second);
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        ids.shrink_to_fit();
        graph.VertexIds_ = std::move(ids);
    }

    const size_t vertexCount = graph.VertexIds_.size();
    const size_t edgeCount = edges.size();
    const std::vector<ui32> lookupTable = BuildLookupTable(graph.VertexIds_);

    // 2. Replace the identifiers by indices and orient every edge from the
    //    smaller index to the larger one.  The weight is packed into the low
    //    byte of the target so that a single sort orders the adjacency lists.
    std::vector<ui32> sources(edgeCount);
    std::vector<ui64> packedTargets(edgeCount);
    graph.AdjacencyOffsets_.assign(vertexCount + 1, 0);
    for (size_t i = 0; i < edgeCount; ++i) {
        const TEdge& edge = edges[i];
        ui32 first = FindVertexIndex(graph.VertexIds_, lookupTable, edge.First);
        ui32 second = FindVertexIndex(graph.VertexIds_, lookupTable, edge.Second);
        if (first > second) {
            std::swap(first, second);
        }
        sources[i] = first;
        packedTargets[i] = (static_cast<ui64>(second) << 8) | edge.Weight;
        ++graph.AdjacencyOffsets_[first + 1];
    }
    std::vector<TEdge>().swap(edges);

    // 3. Distribute the edges over the adjacency lists.
    for (size_t i = 1; i <= vertexCount; ++i) {
        graph.AdjacencyOffsets_[i] += graph.AdjacencyOffsets_[i - 1];
    }
    std::vector<ui64> adjacency(edgeCount);
    {
        std::vector<ui64> cursors(graph.AdjacencyOffsets_.begin(), graph.AdjacencyOffsets_.end() - 1);
        for (size_t i = 0; i < edgeCount; ++i) {
            adjacency[cursors[sources[i]]++] = packedTargets[i];
        }
    }
    std::vector<ui32>().swap(sources);
    std::vector<ui64>().swap(packedTargets);

    // 4. Order every adjacency list and unpack it.
    graph.AdjacencyTargets_.resize(edgeCount);
    graph.EdgeWeights_.resize(edgeCount);
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        const ui64 begin = graph.AdjacencyOffsets_[vertex];
        const ui64 end = graph.AdjacencyOffsets_[vertex + 1];
        SortAdjacencyBucket(adjacency.data() + begin, adjacency.data() + end);
        for (ui64 i = begin; i < end; ++i) {
            const ui32 target = static_cast<ui32>(adjacency[i] >> 8);
            Y_ENSURE(i == begin || target != static_cast<ui32>(adjacency[i - 1] >> 8),
                     "the input graph contains a multiple edge between vertices "
                         << graph.VertexIds_[vertex] << " and " << graph.VertexIds_[target]);
            graph.AdjacencyTargets_[i] = target;
            graph.EdgeWeights_[i] = static_cast<ui8>(adjacency[i] & 0xFF);
        }
    }

    graph.CalculateDegrees();
    return graph;
}

void TGraph::CalculateDegrees() {
    Degrees_.assign(VertexIds_.size(), 0);
    for (size_t vertex = 0; vertex < VertexIds_.size(); ++vertex) {
        const ui64 begin = AdjacencyOffsets_[vertex];
        const ui64 end = AdjacencyOffsets_[vertex + 1];
        Degrees_[vertex] += static_cast<ui32>(end - begin);
        for (ui64 i = begin; i < end; ++i) {
            const ui32 target = AdjacencyTargets_[i];
            if (target != vertex) {
                ++Degrees_[target];
            }
        }
    }
}

} // namespace NGraphCompressor
