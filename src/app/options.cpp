#include "options.h"

#include "../common/exception.h"

namespace NGraphCompressor {

TOptions TOptions::Parse(int argc, const char* const* argv) {
    TOptions options;
    bool modeSeen = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-s" || argument == "-d") {
            Y_ENSURE(!modeSeen, "the mode is specified more than once");
            options.Mode_ = argument == "-s" ? MODE_SERIALIZE : MODE_DESERIALIZE;
            modeSeen = true;
        } else if (argument == "-i" || argument == "-o") {
            Y_ENSURE(i + 1 < argc, "the option " << argument << " requires a file name");
            std::string& destination = argument == "-i" ? options.InputPath_ : options.OutputPath_;
            Y_ENSURE(destination.empty(), "the option " << argument << " is specified more than once");
            destination = argv[++i];
        } else {
            Y_ENSURE(false, "unknown option: " << argument);
        }
    }

    Y_ENSURE(modeSeen, "either -s or -d must be specified");
    Y_ENSURE(!options.InputPath_.empty(), "the input file is not specified");
    Y_ENSURE(!options.OutputPath_.empty(), "the output file is not specified");
    return options;
}

std::string TOptions::GetUsage(const std::string& programName) {
    return "usage:\n"
           "    " + programName + " -s -i input.tsv -o graph.bin    compress a graph\n"
           "    " + programName + " -d -i graph.bin -o output.tsv   restore a graph\n";
}

} // namespace NGraphCompressor
