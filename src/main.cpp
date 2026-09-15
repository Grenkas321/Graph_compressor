#include "app/application.h"
#include "app/options.h"
#include "common/exception.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    using namespace NGraphCompressor;

    try {
        const TOptions options = TOptions::Parse(argc, argv);
        TApplication(options).Run();
    } catch (const TException& error) {
        std::cerr << "error: " << error.what() << "\n"
                  << TOptions::GetUsage(argv[0]);
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
