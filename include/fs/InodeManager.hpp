#pragma once

#include "fs/Inode.hpp"
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

// Base exception thrown for InodeManager operations.
class InodeManagerError : public std::runtime_error {
public:
    explicit InodeManagerError(const std::string& message)
        : std::runtime_error("InodeManager error: " + message) {}
};

// Specialized exception thrown when all allocatable inodes are exhausted.
class NoFreeInodesError : public InodeManagerError {
public:
    NoFreeInodesError()
        : InodeManagerError("No free inodes available") {}
};

class InodeManager {
public:
    // Constructs the manager by reading the Superblock and inode bitmap from an open BlockDevice.
    explicit InodeManager(filler::storage::BlockDevice& device);

    // Constructs the manager with a pre-validated Superblock and reads the inode bitmap from the device.
    InodeManager(filler::storage::BlockDevice& device, const Superblock& sb);

    // Allocates a free inode using the inode bitmap, initializes its metadata,
    // persists the bitmap and inode table entry to disk, and returns the allocated inode ID.
    // Throws NoFreeInodesError if no free inodes remain.
    std::uint64_t allocate_inode(FileType type = FileType::REGULAR, std::uint32_t permissions = 0644);

    // Reads the corresponding inode from disk.
    // Throws InodeManagerError if inode_id is out of range or not allocated.
    [[nodiscard]] Inode get_inode(std::uint64_t inode_id);

    // Updates an existing allocated inode and persists changes to the inode table.
    // Throws InodeManagerError if inode_id is out of range or not allocated.
    void update_inode(std::uint64_t inode_id, const Inode& inode);

    // Releases an allocated inode, clears its inode table metadata, and persists changes.
    // Rejects inode 0 (reserved) and root inode 1 from deletion.
    // Detects and rejects double-free attempts.
    void free_inode(std::uint64_t inode_id);

    // Returns whether an inode ID is currently marked allocated.
    // Throws InodeManagerError if inode_id is out of range.
    [[nodiscard]] bool is_inode_allocated(std::uint64_t inode_id) const;

    // Returns the number of currently available (free) inodes.
    [[nodiscard]] std::uint64_t count_free_inodes() const;

    // Reloads inode bitmap state from disk into memory.
    void reload();

    // Superblock and device accessors
    [[nodiscard]] const Superblock& superblock() const noexcept { return sb_; }
    [[nodiscard]] filler::storage::BlockDevice& device() noexcept { return device_; }

private:
    filler::storage::BlockDevice& device_;
    Superblock sb_;
    std::vector<std::uint8_t> inode_bitmap_data_;
    std::uint64_t next_free_inode_hint_{2}; // Inodes 0 and 1 are reserved / root

    void load_inode_bitmap();
    void persist_inode_bitmap_block(std::uint64_t bitmap_block_idx);
    void set_bitmap_bit(std::uint64_t inode_id, bool value);
    [[nodiscard]] bool get_bitmap_bit(std::uint64_t inode_id) const;

    // Inode table disk offset helpers
    [[nodiscard]] std::uint64_t get_inode_disk_block(std::uint64_t inode_id) const;
    [[nodiscard]] std::size_t get_inode_block_offset(std::uint64_t inode_id) const;
};

} // namespace filler::fs

