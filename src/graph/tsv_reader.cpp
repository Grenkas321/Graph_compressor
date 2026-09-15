#include "tsv_reader.h"

#include "../common/exception.h"
#include "../io/byte_input.h"

namespace NGraphCompressor {

namespace {
    // An estimate of the average line length; used only to preallocate.
    const size_t AVERAGE_LINE_LENGTH = 24;
    const size_t MAX_DIGIT_COUNT = 10;

    ui32 ParseUnsigned(const ui8*& cursor, const ui8* end, ui64 limit) {
        Y_ENSURE(cursor != end && *cursor >= '0' && *cursor <= '9', "a decimal number is expected");
        ui64 value = 0;
        size_t digitCount = 0;
        while (cursor != end && *cursor >= '0' && *cursor <= '9') {
            value = value * 10 + static_cast<ui64>(*cursor - '0');
            ++cursor;
            ++digitCount;
        }
        Y_ENSURE(digitCount <= MAX_DIGIT_COUNT && value <= limit,
                 "the number " << value << " does not fit into the declared range");
        return static_cast<ui32>(value);
    }

    void SkipTab(const ui8*& cursor, const ui8* end) {
        Y_ENSURE(cursor != end && *cursor == '\t', "the numbers must be separated by a single tab");
        ++cursor;
    }
} // namespace

std::vector<TEdge> TTsvReader::Read(const std::string& path) const {
    const std::vector<ui8> content = ReadWholeFile(path);

    std::vector<TEdge> edges;
    edges.reserve(content.size() / AVERAGE_LINE_LENGTH + 1);

    const ui8* cursor = content.data();
    const ui8* const end = cursor + content.size();
    while (cursor != end) {
        if (*cursor == '\n' || *cursor == '\r') {
            ++cursor;
            continue;
        }

        TEdge edge;
        edge.First = ParseUnsigned(cursor, end, 0xFFFFFFFFull);
        SkipTab(cursor, end);
        edge.Second = ParseUnsigned(cursor, end, 0xFFFFFFFFull);
        SkipTab(cursor, end);
        edge.Weight = static_cast<ui8>(ParseUnsigned(cursor, end, 0xFFull));
        Y_ENSURE(cursor == end || *cursor == '\n' || *cursor == '\r', "unexpected trailing data in a line");
        edges.push_back(edge);
    }

    Y_ENSURE(!edges.empty(), "no edges were found in " << path);
    return edges;
}

} // namespace NGraphCompressor
