#include "cli/Shell.hpp"

#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

void print_help(const char* prog_name) {
    std::cout << "Filler Mini File System (Userspace Ext2-like FS)\n"
              << "Usage: " << prog_name << " [OPTIONS] [IMAGE_OR_DEVICE]\n\n"
              << "Options:\n"
              << "  -f, --format       Format the target image or block device before starting the shell\n"
              << "  -h, --help         Display this help message and exit\n\n"
              << "Target:\n"
              << "  IMAGE_OR_DEVICE    Path to a disk image file or block device (default: filler.img)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string image_path = "filler.img";
    bool request_format = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            return 0;
        } else if (arg == "-f" || arg == "--format") {
            request_format = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            print_help(argv[0]);
            return 1;
        } else {
            image_path = arg;
        }
    }

    try {
        filler::cli::Shell shell(image_path);

        if (request_format) {
            shell.format_filesystem(std::cout, std::cin, /*skip_confirmation=*/false);
            if (!shell.is_formatted()) {
                std::cerr << "Format aborted or failed. Exiting.\n";
                return 1;
            }
        }

        if (shell.is_block_device() && shell.device() != nullptr) {
            const double gib = static_cast<double>(shell.device()->size_bytes()) / (1024.0 * 1024.0 * 1024.0);
            std::cout << "Filler Mini File System Shell (block device: " << image_path
                      << ", " << std::fixed << std::setprecision(2) << gib << " GiB)\n";
        } else {
            std::cout << "Filler Mini File System Shell (image: " << image_path << ")\n";
        }
        std::cout << "Type 'help' for available commands or 'exit' to quit.\n";
        shell.run(std::cin, std::cout);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

