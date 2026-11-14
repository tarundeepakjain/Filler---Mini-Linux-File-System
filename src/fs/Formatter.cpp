#include "fs/Formatter.hpp"
#include "storage/BlockDevice.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace filler::fs {

Formatter::Formatter(filler::storage::BlockDevice& device, bool enforce_standard_size)
    : device_(device), enforce_standard_size_(enforce_standard_size) {}

Superblock Formatter::format(std::uint64_t total_inodes) {
    if (!device_.is_open()) {
        throw FormatterError("Cannot format: BlockDevice is not open");
    }

    if (device_.total_blocks() == 0) {
        throw FormatterError("Cannot format: BlockDevice has 0 total blocks");
    }

    if (enforce_standard_size_ && device_.total_blocks() != DEFAULT_TOTAL_BLOCKS) {
        throw FormatterError("Cannot format: BlockDevice size mismatch. Expected " +
                             std::to_string(DEFAULT_TOTAL_BLOCKS) + " blocks (" +
                             std::to_string(DEFAULT_DISK_SIZE) + " bytes), got " +
                             std::to_string(device_.total_blocks()) + " blocks (" +
                             std::to_string(device_.size_bytes()) + " bytes)");
    }

    if (total_inodes == 0) {
        total_inodes = calculate_default_inodes(device_.total_blocks());
    }

    // Generate Superblock with disk layout calculated by create_default()
    Superblock sb = Superblock::create_default(device_.total_blocks(), total_inodes);

    // Initialize disk metadata regions
    initialize_block_bitmap(sb);
    initialize_inode_bitmap(sb);
    zero_inode_table(sb);

    // Commit the Superblock to block 0
    sb.write(device_, /*check_device_size=*/true);

    return sb;
}

Superblock Formatter::format(filler::storage::BlockDevice& device,
                             std::uint64_t total_inodes,
                             bool enforce_standard_size) {
    Formatter formatter(device, enforce_standard_size);
    return formatter.format(total_inodes);
}

void Formatter::initialize_block_bitmap(const Superblock& sb) {
    const std::uint64_t bits_per_block = static_cast<std::uint64_t>(sb.block_size) * 8ULL;
    std::vector<std::uint8_t> buffer(sb.block_size, 0);

    for (std::uint64_t b = 0; b < sb.block_bitmap_blocks; ++b) {
        std::fill(buffer.begin(), buffer.end(), 0);
        const std::uint64_t block_start_bit = b * bits_per_block;
        const std::uint64_t block_end_bit = block_start_bit + bits_per_block;

        // All blocks before sb.data_region_start are metadata blocks and must be marked allocated (1).
        if (block_start_bit < sb.data_region_start) {
            const std::uint64_t allocated_end = std::min(block_end_bit, sb.data_region_start);
            for (std::uint64_t blk = block_start_bit; blk < allocated_end; ++blk) {
                const std::uint64_t local_bit = blk - block_start_bit;
                const std::size_t byte_idx = static_cast<std::size_t>(local_bit / 8ULL);
                const std::size_t bit_idx = static_cast<std::size_t>(local_bit % 8ULL);
                buffer[byte_idx] |= static_cast<std::uint8_t>(1U << bit_idx);
            }
        }

        device_.write_block(sb.block_bitmap_start + b, buffer.data(), buffer.size());
    }
}

void Formatter::initialize_inode_bitmap(const Superblock& sb) {
    const std::uint64_t bits_per_block = static_cast<std::uint64_t>(sb.block_size) * 8ULL;
    std::vector<std::uint8_t> buffer(sb.block_size, 0);

    for (std::uint64_t b = 0; b < sb.inode_bitmap_blocks; ++b) {
        std::fill(buffer.begin(), buffer.end(), 0);
        const std::uint64_t block_start_bit = b * bits_per_block;
        const std::uint64_t block_end_bit = block_start_bit + bits_per_block;

        // Reserve inode 0
        if (Superblock::RESERVED_INODE >= block_start_bit && Superblock::RESERVED_INODE < block_end_bit) {
            const std::uint64_t local_bit = Superblock::RESERVED_INODE - block_start_bit;
            const std::size_t byte_idx = static_cast<std::size_t>(local_bit / 8ULL);
            const std::size_t bit_idx = static_cast<std::size_t>(local_bit % 8ULL);
            buffer[byte_idx] |= static_cast<std::uint8_t>(1U << bit_idx);
        }

        // Allocate root inode 1
        if (sb.root_inode >= block_start_bit && sb.root_inode < block_end_bit) {
            const std::uint64_t local_bit = sb.root_inode - block_start_bit;
            const std::size_t byte_idx = static_cast<std::size_t>(local_bit / 8ULL);
            const std::size_t bit_idx = static_cast<std::size_t>(local_bit % 8ULL);
            buffer[byte_idx] |= static_cast<std::uint8_t>(1U << bit_idx);
        }

        device_.write_block(sb.inode_bitmap_start + b, buffer.data(), buffer.size());
    }
}

void Formatter::zero_inode_table(const Superblock& sb) {
    const std::vector<std::uint8_t> zero_buffer(sb.block_size, 0);

    for (std::uint64_t b = 0; b < sb.inode_table_blocks; ++b) {
        device_.write_block(sb.inode_table_start + b, zero_buffer.data(), zero_buffer.size());
    }
}

} // namespace filler::fs

