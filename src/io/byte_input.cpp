#include "byte_input.h"

#include "../common/exception.h"

#include <cstdio>

namespace NGraphCompressor {

std::vector<ui8> ReadWholeFile(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    Y_ENSURE(file != nullptr, "cannot open file for reading: " << path);

    std::vector<ui8> result;
    try {
        Y_ENSURE(std::fseek(file, 0, SEEK_END) == 0, "cannot seek in file: " << path);
        const long size = std::ftell(file);
        Y_ENSURE(size >= 0, "cannot get the size of file: " << path);
        Y_ENSURE(std::fseek(file, 0, SEEK_SET) == 0, "cannot seek in file: " << path);

        result.resize(static_cast<size_t>(size));
        if (!result.empty()) {
            const size_t read = std::fread(result.data(), 1, result.size(), file);
            Y_ENSURE(read == result.size(), "cannot read " << result.size() << " bytes from " << path);
        }
    } catch (...) {
        std::fclose(file);
        throw;
    }

    std::fclose(file);
    return result;
}

} // namespace NGraphCompressor
