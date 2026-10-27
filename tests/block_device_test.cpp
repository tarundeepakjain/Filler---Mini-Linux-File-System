
#include "storage/BlockDevice.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        filler::storage::BlockDevice device("filler.img");

        std::cout << "Image size: " << device.size_bytes()
                  << " bytes\n";
        std::cout << "Total blocks: " << device.total_blocks()
                  << '\n';

        constexpr std::uint64_t test_block = 0;

        std::array<char, 4096> written{};
        std::array<char, 4096> read{};

        written[0] = 'F';
        written[1] = 'I';
        written[2] = 'L';
        written[3] = 'L';

        device.write_block(test_block, written.data(), written.size());
        device.read_block(test_block, read.data(), read.size());

        if (written == read) {
            std::cout << "PASS: Block read/write test succeeded!\n";
        } else {
            std::cerr << "FAIL: Data mismatch!\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
