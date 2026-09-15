#pragma once

#include "options.h"

namespace NGraphCompressor {

// Ties the components together: reading, converting and writing a graph.
class TApplication {
public:
    explicit TApplication(const TOptions& options)
        : Options_(options)
    {
    }

    void Run() const;

private:
    void Serialize() const;
    void Deserialize() const;

private:
    const TOptions& Options_;
};

} // namespace NGraphCompressor
