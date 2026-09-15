#pragma once

#include <string>

namespace NGraphCompressor {

// What the program is asked to do.
enum EMode {
    MODE_SERIALIZE,
    MODE_DESERIALIZE,
};

// Command line of the program:
//     run -s -i input.tsv -o graph.bin
//     run -d -i graph.bin -o output.tsv
class TOptions {
public:
    static TOptions Parse(int argc, const char* const* argv);
    static std::string GetUsage(const std::string& programName);

    EMode GetMode() const noexcept {
        return Mode_;
    }

    const std::string& GetInputPath() const noexcept {
        return InputPath_;
    }

    const std::string& GetOutputPath() const noexcept {
        return OutputPath_;
    }

private:
    TOptions() = default;

private:
    EMode Mode_ = MODE_SERIALIZE;
    std::string InputPath_;
    std::string OutputPath_;
};

} // namespace NGraphCompressor
