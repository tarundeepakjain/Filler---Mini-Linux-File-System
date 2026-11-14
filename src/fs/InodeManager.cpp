#include "fs/InodeManager.hpp"
#include "storage/BlockDevice.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace filler::fs {

InodeManager::InodeManager(filler::storage::BlockDevice& device)
    : device_(device) {
    if (!device_.is_open()) {
        throw InodeManagerError("Cannot create InodeManager: BlockDevice is not open");
    }
    sb_ = Superblock::read(device_);
    load_inode_bitmap();
}

InodeManager::InodeManager(filler::storage::BlockDevice& device, const Superblock& sb)
    : device_(device), sb_(sb) {
    if (!device_.is_open()) {
        throw InodeManagerError("Cannot create InodeManager: BlockDevice is not open");
    }
    sb_.validate();
    load_inode_bitmap();
}

void InodeManager::load_inode_bitmap() {
    const std::size_t total_bitmap_bytes = static_cast<std::size_t>(sb_.inode_bitmap_blocks * sb_.block_size);
    inode_bitmap_data_.assign(total_bitmap_bytes, static_cast<std::uint8_t>(0));

    for (std::uint64_t b = 0; b < sb_.inode_bitmap_blocks; ++b) {
        device_.read_block(sb_.inode_bitmap_start + b,
                           inode_bitmap_data_.data() + (b * sb_.block_size),
                           sb_.block_size);
    }

    next_free_inode_hint_ = 2; // Inodes 0 and 1 are reserved / root
}

void InodeManager::reload() {
    if (!device_.is_open()) {
        throw InodeManagerError("Cannot reload InodeManager: BlockDevice is not open");
    }
    sb_ = Superblock::read(device_);
    load_inode_bitmap();
}

bool InodeManager::get_bitmap_bit(std::uint64_t inode_id) const {
    const std::uint64_t bits_per_block = static_cast<std::uint64_t>(sb_.block_size) * 8ULL;
    const std::uint64_t bitmap_block_idx = inode_id / bits_per_block;
    const std::uint64_t local_bit = inode_id % bits_per_block;
    const std::size_t byte_offset = static_cast<std::size_t>((bitmap_block_idx * sb_.block_size) + (local_bit / 8ULL));
    const std::size_t bit_offset = static_cast<std::size_t>(local_bit % 8ULL);

    return (inode_bitmap_data_[byte_offset] & static_cast<std::uint8_t>(1U << bit_offset)) != 0;
}

void InodeManager::set_bitmap_bit(std::uint64_t inode_id, bool value) {
    const std::uint64_t bits_per_block = static_cast<std::uint64_t>(sb_.block_size) * 8ULL;
    const std::uint64_t bitmap_block_idx = inode_id / bits_per_block;
    const std::uint64_t local_bit = inode_id % bits_per_block;
    const std::size_t byte_offset = static_cast<std::size_t>((bitmap_block_idx * sb_.block_size) + (local_bit / 8ULL));
    const std::size_t bit_offset = static_cast<std::size_t>(local_bit % 8ULL);

    if (value) {
        inode_bitmap_data_[byte_offset] |= static_cast<std::uint8_t>(1U << bit_offset);
    } else {
        inode_bitmap_data_[byte_offset] &= static_cast<std::uint8_t>(~(1U << bit_offset));
    }
}

void InodeManager::persist_inode_bitmap_block(std::uint64_t bitmap_block_idx) {
    const std::uint8_t* block_ptr = inode_bitmap_data_.data() + (bitmap_block_idx * sb_.block_size);
    const std::uint64_t disk_block_id = sb_.inode_bitmap_start + bitmap_block_idx;
    device_.write_block(disk_block_id, block_ptr, sb_.block_size);
}

std::uint64_t InodeManager::get_inode_disk_block(std::uint64_t inode_id) const {
    const std::uint64_t inodes_per_block = sb_.block_size / sb_.inode_size;
    const std::uint64_t block_offset = inode_id / inodes_per_block;
    return sb_.inode_table_start + block_offset;
}

std::size_t InodeManager::get_inode_block_offset(std::uint64_t inode_id) const {
    const std::uint64_t inodes_per_block = sb_.block_size / sb_.inode_size;
    const std::uint64_t local_index = inode_id % inodes_per_block;
    return static_cast<std::size_t>(local_index * sb_.inode_size);
}

std::uint64_t InodeManager::allocate_inode(FileType type, std::uint32_t permissions) {
    if (!device_.is_open()) {
        throw InodeManagerError("Cannot allocate inode: BlockDevice is not open");
    }

    // Inode 0 is reserved, inode 1 is reserved for root. Allocatable inodes start at 2.
    std::uint64_t candidate = std::max(next_free_inode_hint_, static_cast<std::uint64_t>(2));
    bool found = false;

    for (std::uint64_t ino = candidate; ino < sb_.total_inodes; ++ino) {
        if (!get_bitmap_bit(ino)) {
            candidate = ino;
            found = true;
            break;
        }
    }

    if (!found && candidate > 2) {
        for (std::uint64_t ino = 2; ino < candidate; ++ino) {
            if (!get_bitmap_bit(ino)) {
                candidate = ino;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        throw NoFreeInodesError();
    }

    // Mark as allocated in bitmap
    set_bitmap_bit(candidate, true);
    const std::uint64_t bits_per_block = static_cast<std::uint64_t>(sb_.block_size) * 8ULL;
    persist_inode_bitmap_block(candidate / bits_per_block);

    // Initialize Inode structure
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();

    Inode inode;
    inode.inode_id = candidate;
    inode.type = type;
    inode.permissions = permissions;
    inode.uid = 0;
    inode.gid = 0;
    inode.size_bytes = 0;
    inode.blocks_count = 0;
    inode.atime_sec = now;
    inode.mtime_sec = now;
    inode.ctime_sec = now;

    // Persist to inode table without overwriting adjacent entries
    const std::uint64_t disk_block = get_inode_disk_block(candidate);
    const std::size_t byte_offset = get_inode_block_offset(candidate);

    std::vector<std::uint8_t> block_buf(sb_.block_size);
    device_.read_block(disk_block, block_buf.data(), block_buf.size());
    inode.serialize(block_buf.data() + byte_offset, sb_.inode_size);
    device_.write_block(disk_block, block_buf.data(), block_buf.size());

    // Update Superblock free_inodes count and persist
    if (sb_.free_inodes > 0) {
        --sb_.free_inodes;
        sb_.write(device_);
    }

    next_free_inode_hint_ = candidate + 1;
    return candidate;
}

Inode InodeManager::get_inode(std::uint64_t inode_id) {
    if (!device_.is_open()) {
        throw InodeManagerError("Cannot get inode: BlockDevice is not open");
    }

    if (inode_id >= sb_.total_inodes) {
        throw InodeManagerError("Cannot get inode: inode ID " + std::to_string(inode_id) +
                                " is out of range (total inodes: " + std::to_string(sb_.total_inodes) + ")");
    }

    if (!is_inode_allocated(inode_id)) {
        throw InodeManagerError("Cannot get inode " + std::to_string(inode_id) + ": inode is not allocated");
    }

    const std::uint64_t disk_block = get_inode_disk_block(inode_id);
    const std::size_t byte_offset = get_inode_block_offset(inode_id);

    std::vector<std::uint8_t> block_buf(sb_.block_size);
    device_.read_block(disk_block, block_buf.data(), block_buf.size());

    return Inode::deserialize(block_buf.data() + byte_offset, sb_.inode_size);
}

void InodeManager::update_inode(std::uint64_t inode_id, const Inode& inode) {
    if (!device_.is_open()) {
        throw InodeManagerError("Cannot update inode: BlockDevice is not open");
    }

    if (inode_id >= sb_.total_inodes) {
        throw InodeManagerError("Cannot update inode: inode ID " + std::to_string(inode_id) +
                                " is out of range (total inodes: " + std::to_string(sb_.total_inodes) + ")");
    }

    if (!is_inode_allocated(inode_id)) {
        throw InodeManagerError("Cannot update inode " + std::to_string(inode_id) + ": inode is not allocated");
    }

    const std::uint64_t disk_block = get_inode_disk_block(inode_id);
    const std::size_t byte_offset = get_inode_block_offset(inode_id);

    std::vector<std::uint8_t> block_buf(sb_.block_size);
    device_.read_block(disk_block, block_buf.data(), block_buf.size());

    Inode to_write = inode;
    to_write.inode_id = inode_id; // Guarantee inode_id integrity
    to_write.serialize(block_buf.data() + byte_offset, sb_.inode_size);

    device_.write_block(disk_block, block_buf.data(), block_buf.size());
}

void InodeManager::free_inode(std::uint64_t inode_id) {
    if (!device_.is_open()) {
        throw InodeManagerError("Cannot free inode: BlockDevice is not open");
    }

    if (inode_id >= sb_.total_inodes) {
        throw InodeManagerError("Cannot free inode: inode ID " + std::to_string(inode_id) +
                                " is out of range (total inodes: " + std::to_string(sb_.total_inodes) + ")");
    }

    // Protection of reserved inode 0 and root inode 1
    if (inode_id == Superblock::RESERVED_INODE) {
        throw InodeManagerError("Cannot free reserved inode 0");
    }
    if (inode_id == sb_.root_inode) {
        throw InodeManagerError("Cannot free root inode 1");
    }

    // Double-free detection
    if (!get_bitmap_bit(inode_id)) {
        throw InodeManagerError("Double free detected: inode " + std::to_string(inode_id) + " is not allocated");
    }

    // Mark as free in inode bitmap
    set_bitmap_bit(inode_id, false);
    const std::uint64_t bits_per_block = static_cast<std::uint64_t>(sb_.block_size) * 8ULL;
    persist_inode_bitmap_block(inode_id / bits_per_block);

    // Clear inode metadata in inode table
    const std::uint64_t disk_block = get_inode_disk_block(inode_id);
    const std::size_t byte_offset = get_inode_block_offset(inode_id);

    std::vector<std::uint8_t> block_buf(sb_.block_size);
    device_.read_block(disk_block, block_buf.data(), block_buf.size());
    std::memset(block_buf.data() + byte_offset, 0, sb_.inode_size);
    device_.write_block(disk_block, block_buf.data(), block_buf.size());

    // Update Superblock free_inodes count and persist
    if (sb_.free_inodes < sb_.total_inodes - 2) {
        ++sb_.free_inodes;
        sb_.write(device_);
    }

    if (inode_id < next_free_inode_hint_) {
        next_free_inode_hint_ = inode_id;
    }
}

bool InodeManager::is_inode_allocated(std::uint64_t inode_id) const {
    if (!device_.is_open()) {
        throw InodeManagerError("Cannot check allocation status: BlockDevice is not open");
    }

    if (inode_id >= sb_.total_inodes) {
        throw InodeManagerError("Cannot check allocation status: inode ID " + std::to_string(inode_id) +
                                " is out of range (total inodes: " + std::to_string(sb_.total_inodes) + ")");
    }

    return get_bitmap_bit(inode_id);
}

std::uint64_t InodeManager::count_free_inodes() const {
    if (!device_.is_open()) {
        throw InodeManagerError("Cannot count free inodes: BlockDevice is not open");
    }

    std::uint64_t free_count = 0;
    // Inodes 0 and 1 are reserved; allocatable pool is [2, total_inodes)
    for (std::uint64_t ino = 2; ino < sb_.total_inodes; ++ino) {
        if (!get_bitmap_bit(ino)) {
            ++free_count;
        }
    }
    return free_count;
}

} // namespace filler::fs

