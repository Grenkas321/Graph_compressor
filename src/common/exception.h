#pragma once

#include <sstream>
#include <stdexcept>
#include <string>

namespace NGraphCompressor {

// Base class for all errors reported by the compressor.
class TException: public std::runtime_error {
public:
    explicit TException(const std::string& message)
        : std::runtime_error(message)
    {
    }
};

} // namespace NGraphCompressor

// Throws TException with a formatted message unless the condition holds.
// Usage: Y_ENSURE(size > 0, "empty input file: " << path);
#define Y_ENSURE(condition, message)                                   \
    do {                                                               \
        if (!(condition)) {                                            \
            std::ostringstream ensureStream;                           \
            ensureStream << message;                                   \
            throw ::NGraphCompressor::TException(ensureStream.str());  \
        }                                                              \
    } while (false)
