#include "fs/BlockBitmap.hpp"
#include "storage/BlockDevice.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace filler::fs {

BlockBitmap::BlockBitmap(filler::storage::BlockDevice& device)
    : device_(device) {
    if (!device_.is_open()) {
        throw BlockBitmapError("Cannot create BlockBitmap: BlockDevice is not open");
    }
    sb_ = Superblock::read(device_);
    load_from_device();
}

BlockBitmap::BlockBitmap(filler::storage::BlockDevice& device, const Superblock& sb)
    : device_(device), sb_(sb) {
    if (!device_.is_open()) {
        throw BlockBitmapError("Cannot create BlockBitmap: BlockDevice is not open");
    }
    sb_.validate();
    load_from_device();
}

void BlockBitmap::load_from_device() {
    const std::size_t total_bitmap_bytes = static_cast<std::size_t>(sb_.block_bitmap_blocks * sb_.block_size);
    bitmap_data_.assign(total_bitmap_bytes, static_cast<std::uint8_t>(0));

    for (std::uint64_t b = 0; b < sb_.block_bitmap_blocks; ++b) {
        device_.read_block(sb_.block_bitmap_start + b,
                           bitmap_data_.data() + (b * sb_.block_size),
                           sb_.block_size);
    }

    next_free_hint_ = sb_.data_region_start;
}

void BlockBitmap::reload() {
    if (!device_.is_open()) {
        throw BlockBitmapError("Cannot reload BlockBitmap: BlockDevice is not open");
    }
    sb_ = Superblock::read(device_);
    load_from_device();
}

bool BlockBitmap::get_bit(std::uint64_t block_id) const {
    const std::uint64_t bits_per_block = static_cast<std::uint64_t>(sb_.block_size) * 8ULL;
    const std::uint64_t bitmap_block_idx = block_id / bits_per_block;
    const std::uint64_t local_bit = block_id % bits_per_block;
    const std::size_t byte_offset = static_cast<std::size_t>((bitmap_block_idx * sb_.block_size) + (local_bit / 8ULL));
    const std::size_t bit_offset = static_cast<std::size_t>(local_bit % 8ULL);

    return (bitmap_data_[byte_offset] & static_cast<std::uint8_t>(1U << bit_offset)) != 0;
}

void BlockBitmap::set_bit(std::uint64_t block_id, bool value) {
    const std::uint64_t bits_per_block = static_cast<std::uint64_t>(sb_.block_size) * 8ULL;
    const std::uint64_t bitmap_block_idx = block_id / bits_per_block;
    const std::uint64_t local_bit = block_id % bits_per_block;
    const std::size_t byte_offset = static_cast<std::size_t>((bitmap_block_idx * sb_.block_size) + (local_bit / 8ULL));
    const std::size_t bit_offset = static_cast<std::size_t>(local_bit % 8ULL);

    if (value) {
        bitmap_data_[byte_offset] |= static_cast<std::uint8_t>(1U << bit_offset);
    } else {
        bitmap_data_[byte_offset] &= static_cast<std::uint8_t>(~(1U << bit_offset));
    }
}

void BlockBitmap::persist_bitmap_block(std::uint64_t bitmap_block_idx) {
    const std::uint8_t* block_ptr = bitmap_data_.data() + (bitmap_block_idx * sb_.block_size);
    const std::uint64_t disk_block_id = sb_.block_bitmap_start + bitmap_block_idx;
    device_.write_block(disk_block_id, block_ptr, sb_.block_size);
}

std::uint64_t BlockBitmap::allocate() {
    if (!device_.is_open()) {
        throw BlockBitmapError("Cannot allocate block: BlockDevice is not open");
    }

    const std::uint64_t data_start = sb_.data_region_start;
    const std::uint64_t data_end = sb_.total_blocks;

    std::uint64_t candidate = std::max(next_free_hint_, data_start);
    bool found = false;

    // Scan from candidate hint to data_end
    for (std::uint64_t blk = candidate; blk < data_end; ++blk) {
        if (!get_bit(blk)) {
            candidate = blk;
            found = true;
            break;
        }
    }

    // If not found, wrap around to data_start to search earlier freed blocks
    if (!found && candidate > data_start) {
        for (std::uint64_t blk = data_start; blk < candidate; ++blk) {
            if (!get_bit(blk)) {
                candidate = blk;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        throw NoFreeBlocksError();
    }

    // Mark candidate as allocated
    set_bit(candidate, true);

    // Persist modified bitmap block to disk
    const std::uint64_t bits_per_block = static_cast<std::uint64_t>(sb_.block_size) * 8ULL;
    const std::uint64_t bitmap_block_idx = candidate / bits_per_block;
    persist_bitmap_block(bitmap_block_idx);

    // Update Superblock free_blocks and persist to disk
    if (sb_.free_blocks > 0) {
        --sb_.free_blocks;
        sb_.write(device_);
    }

    next_free_hint_ = candidate + 1;
    return candidate;
}

void BlockBitmap::free(std::uint64_t block_id) {
    if (!device_.is_open()) {
        throw BlockBitmapError("Cannot free block: BlockDevice is not open");
    }

    // Bounds check
    if (block_id >= sb_.total_blocks) {
        throw BlockBitmapError("Cannot free block: block ID " + std::to_string(block_id) +
                               " is out of range (total blocks: " + std::to_string(sb_.total_blocks) + ")");
    }

    // Never allow metadata blocks to be freed
    if (block_id < sb_.data_region_start) {
        throw BlockBitmapError("Cannot free metadata block " + std::to_string(block_id) +
                               " (metadata range: 0 to " + std::to_string(sb_.data_region_start - 1) + ")");
    }

    // Detect double-free attempts
    if (!get_bit(block_id)) {
        throw BlockBitmapError("Double-free detected: block " + std::to_string(block_id) + " is already free");
    }

    // Mark as free
    set_bit(block_id, false);

    // Persist modified bitmap block to disk
    const std::uint64_t bits_per_block = static_cast<std::uint64_t>(sb_.block_size) * 8ULL;
    const std::uint64_t bitmap_block_idx = block_id / bits_per_block;
    persist_bitmap_block(bitmap_block_idx);

    // Update Superblock free_blocks and persist to disk
    if (sb_.free_blocks < sb_.data_region_blocks) {
        ++sb_.free_blocks;
        sb_.write(device_);
    }

    // Update allocation search hint to reuse this newly freed block
    if (block_id < next_free_hint_) {
        next_free_hint_ = block_id;
    }
}

bool BlockBitmap::is_allocated(std::uint64_t block_id) const {
    if (!device_.is_open()) {
        throw BlockBitmapError("Cannot check allocation status: BlockDevice is not open");
    }

    if (block_id >= sb_.total_blocks) {
        throw BlockBitmapError("Cannot check allocation status: block ID " + std::to_string(block_id) +
                               " is out of range (total blocks: " + std::to_string(sb_.total_blocks) + ")");
    }

    return get_bit(block_id);
}

std::uint64_t BlockBitmap::count_free_blocks() const {
    if (!device_.is_open()) {
        throw BlockBitmapError("Cannot count free blocks: BlockDevice is not open");
    }

    std::uint64_t free_count = 0;
    const std::uint64_t data_start = sb_.data_region_start;
    const std::uint64_t data_end = sb_.total_blocks;

    for (std::uint64_t blk = data_start; blk < data_end; ++blk) {
        if (!get_bit(blk)) {
            ++free_count;
        }
    }

    return free_count;
}

void BlockBitmap::initialize() {
    if (!device_.is_open()) {
        throw BlockBitmapError("Cannot initialize block bitmap: BlockDevice is not open");
    }

    std::fill(bitmap_data_.begin(), bitmap_data_.end(), static_cast<std::uint8_t>(0));

    // Metadata blocks [0, sb_.data_region_start) must be marked allocated (1)
    for (std::uint64_t blk = 0; blk < sb_.data_region_start; ++blk) {
        set_bit(blk, true);
    }

    // Persist all bitmap blocks
    for (std::uint64_t b = 0; b < sb_.block_bitmap_blocks; ++b) {
        persist_bitmap_block(b);
    }

    // Reset free_blocks in superblock and persist
    sb_.free_blocks = sb_.data_region_blocks;
    sb_.write(device_);

    next_free_hint_ = sb_.data_region_start;
}

} // namespace filler::fs

