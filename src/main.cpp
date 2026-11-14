#include "cli/Shell.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::string image_path = "filler.img";
    if (argc > 1) {
        image_path = argv[1];
    }

    try {
        filler::cli::Shell shell(image_path);
        std::cout << "Filler Mini File System Shell (image: " << image_path << ")\n";
        std::cout << "Type 'help' for available commands or 'exit' to quit.\n";
        shell.run(std::cin, std::cout);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

