#pragma once

#include "../common/exception.h"
#include "../common/types.h"

#include <string>
#include <vector>

namespace NGraphCompressor {

// Reads the whole file into memory.  The inputs of this task never exceed a
// few hundred megabytes, so a single allocation is both the simplest and the
// fastest option.
std::vector<ui8> ReadWholeFile(const std::string& path);

// Sequential reader over a memory block owned by somebody else.
class TByteInput {
public:
    TByteInput(const ui8* data, size_t size) noexcept
        : Data_(data)
        , Size_(size)
        , Position_(0)
    {
    }

    ui8 ReadByte() {
        Y_ENSURE(Position_ < Size_, "unexpected end of the compressed stream");
        return Data_[Position_++];
    }

    // Reads a byte without bounds checking; the range decoder may look a few
    // bytes past the end of the stream, which is harmless for the result.
    ui8 ReadByteOrZero() noexcept {
        return Position_ < Size_ ? Data_[Position_++] : 0;
    }

    size_t GetPosition() const noexcept {
        return Position_;
    }

private:
    const ui8* Data_;
    size_t Size_;
    size_t Position_;
};

} // namespace NGraphCompressor
