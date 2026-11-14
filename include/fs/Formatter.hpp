#pragma once

#include "fs/Superblock.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace filler::storage {
class BlockDevice;
}

namespace filler::fs {

// Custom exception thrown when filesystem formatting operations fail.
class FormatterError : public std::runtime_error {
public:
    explicit FormatterError(const std::string& message)
        : std::runtime_error("Formatter error: " + message) {}
};

class Formatter {
public:
    // Fixed project parameters
    static constexpr std::size_t DEFAULT_DISK_SIZE = 64ULL * 1024ULL * 1024ULL; // 64 MiB (67,108,864 bytes)
    static constexpr std::uint64_t DEFAULT_TOTAL_BLOCKS = 16384;
    static constexpr std::uint64_t DEFAULT_TOTAL_INODES = 256;

    // Calculates default inode count scaling with capacity (1 inode per 256 KiB, min 256, multiple of 16)
    static std::uint64_t calculate_default_inodes(std::uint64_t total_blocks) noexcept {
        if (total_blocks <= DEFAULT_TOTAL_BLOCKS) {
            return DEFAULT_TOTAL_INODES;
        }
        std::uint64_t inodes = std::max<std::uint64_t>(DEFAULT_TOTAL_INODES, total_blocks / 64ULL);
        return ((inodes + 15ULL) / 16ULL) * 16ULL;
    }

    // Constructs a formatter targeting an existing BlockDevice.
    explicit Formatter(filler::storage::BlockDevice& device,
                       bool enforce_standard_size = true);

    // Formats the underlying BlockDevice and returns the created Superblock.
    // If total_inodes == 0, calculates default inodes based on capacity.
    Superblock format(std::uint64_t total_inodes = 0);

    // Static convenience method that formats the given BlockDevice.
    static Superblock format(filler::storage::BlockDevice& device,
                             std::uint64_t total_inodes = 0,
                             bool enforce_standard_size = true);

private:
    filler::storage::BlockDevice& device_;
    bool enforce_standard_size_{true};

    void initialize_block_bitmap(const Superblock& sb);
    void initialize_inode_bitmap(const Superblock& sb);
    void zero_inode_table(const Superblock& sb);
};

} // namespace filler::fs

