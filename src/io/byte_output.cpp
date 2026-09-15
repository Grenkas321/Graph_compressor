#include "byte_output.h"

#include "../common/exception.h"

namespace NGraphCompressor {

TByteOutput::TByteOutput(const std::string& path, size_t bufferSize)
    : File_(std::fopen(path.c_str(), "wb"))
    , Path_(path)
    , Buffer_(bufferSize)
    , Position_(0)
    , WrittenByteCount_(0)
{
    Y_ENSURE(File_ != nullptr, "cannot open file for writing: " << path);
}

TByteOutput::~TByteOutput() {
    if (File_ != nullptr) {
        std::fclose(File_);
    }
}

void TByteOutput::Write(const ui8* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        WriteByte(data[i]);
    }
}

void TByteOutput::Finish() {
    FlushBuffer();
    Y_ENSURE(std::fflush(File_) == 0, "cannot flush file: " << Path_);
}

void TByteOutput::FlushBuffer() {
    if (Position_ == 0) {
        return;
    }
    const size_t written = std::fwrite(Buffer_.data(), 1, Position_, File_);
    Y_ENSURE(written == Position_, "cannot write " << Position_ << " bytes to " << Path_);
    WrittenByteCount_ += Position_;
    Position_ = 0;
}

} // namespace NGraphCompressor
