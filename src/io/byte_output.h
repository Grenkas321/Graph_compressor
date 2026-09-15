#pragma once

#include "../common/types.h"

#include <cstdio>
#include <string>
#include <vector>

namespace NGraphCompressor {

// Buffered sink of bytes backed by a regular file.
//
// The class owns the file handle, closes it in the destructor and is therefore
// neither copyable nor assignable.  WriteByte() is the hot path of the range
// coder, so it is kept inline and branch-free apart from the buffer check.
class TByteOutput {
public:
    explicit TByteOutput(const std::string& path, size_t bufferSize = DEFAULT_BUFFER_SIZE);
    TByteOutput(const TByteOutput&) = delete;
    TByteOutput& operator=(const TByteOutput&) = delete;
    ~TByteOutput();

    void WriteByte(ui8 value) {
        if (Position_ == Buffer_.size()) {
            FlushBuffer();
        }
        Buffer_[Position_++] = value;
    }

    void Write(const ui8* data, size_t size);

    // Returns a pointer to at least `size` writable bytes of the internal
    // buffer.  The caller fills them and reports the used amount to Commit().
    // `size` must not exceed the buffer size.
    ui8* Reserve(size_t size) {
        if (Buffer_.size() - Position_ < size) {
            FlushBuffer();
        }
        return Buffer_.data() + Position_;
    }

    void Commit(size_t size) noexcept {
        Position_ += size;
    }

    // Flushes the internal buffer to the operating system.  Must be called
    // before the destructor if write errors have to be reported.
    void Finish();

    ui64 GetWrittenByteCount() const noexcept {
        return WrittenByteCount_ + Position_;
    }

private:
    static const size_t DEFAULT_BUFFER_SIZE = 1u << 20;

    void FlushBuffer();

private:
    std::FILE* File_;
    std::string Path_;
    std::vector<ui8> Buffer_;
    size_t Position_;
    ui64 WrittenByteCount_;
};

} // namespace NGraphCompressor
