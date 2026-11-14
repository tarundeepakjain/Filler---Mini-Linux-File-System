#pragma once

#include "fs/Superblock.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace filler::storage {
class BlockDevice;
}

namespace filler::fs {

// Base exception thrown for block bitmap manager operations.
class BlockBitmapError : public std::runtime_error {
public:
    explicit BlockBitmapError(const std::string& message)
        : std::runtime_error("BlockBitmap error: " + message) {}
};

// Specialized exception thrown when no free blocks are available.
class NoFreeBlocksError : public BlockBitmapError {
public:
    NoFreeBlocksError()
        : BlockBitmapError("No free data blocks available") {}
};

class BlockBitmap {
public:
    // Constructs the manager by reading the Superblock and block bitmap from an open BlockDevice.
    explicit BlockBitmap(filler::storage::BlockDevice& device);

    // Constructs the manager with a pre-validated Superblock and reads the bitmap from the device.
    BlockBitmap(filler::storage::BlockDevice& device, const Superblock& sb);

    // Finds the first available data block, marks it allocated,
    // persists the update to disk, and returns the allocated block ID.
    // Throws NoFreeBlocksError if no blocks are available.
    std::uint64_t allocate();

    // Releases an allocated data block and persists the update.
    // Throws BlockBitmapError if block_id is invalid, refers to a metadata block,
    // or has already been freed (double-free).
    void free(std::uint64_t block_id);

    // Returns whether a valid block is currently allocated.
    // Throws BlockBitmapError if block_id is out of range.
    [[nodiscard]] bool is_allocated(std::uint64_t block_id) const;

    // Returns the number of currently available (free) data blocks.
    [[nodiscard]] std::uint64_t count_free_blocks() const;

    // Initializes bitmap state consistently with Formatter:
    // Metadata blocks [0, data_region_start) are marked allocated (1).
    // Data blocks [data_region_start, total_blocks) are marked free (0).
    // Persists the initial bitmap and updates Superblock free_blocks count.
    void initialize();

    // Reloads bitmap state from disk into memory.
    void reload();

    // Superblock and device accessors
    [[nodiscard]] const Superblock& superblock() const noexcept { return sb_; }
    [[nodiscard]] filler::storage::BlockDevice& device() noexcept { return device_; }

private:
    filler::storage::BlockDevice& device_;
    Superblock sb_;
    std::vector<std::uint8_t> bitmap_data_;
    std::uint64_t next_free_hint_{0};

    void load_from_device();
    void persist_bitmap_block(std::uint64_t bitmap_block_idx);
    void set_bit(std::uint64_t block_id, bool value);
    [[nodiscard]] bool get_bit(std::uint64_t block_id) const;
};

} // namespace filler::fs

