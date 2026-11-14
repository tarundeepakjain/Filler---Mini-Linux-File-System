#include "fs/DirectoryManager.hpp"
#include "storage/BlockDevice.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace filler::fs {

using filler::storage::BlockDevice;

namespace {

inline void write_u32_le(std::uint8_t* dst, std::uint32_t val) noexcept {
    dst[0] = static_cast<std::uint8_t>(val & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((val >> 8U) & 0xFFU);
    dst[2] = static_cast<std::uint8_t>((val >> 16U) & 0xFFU);
    dst[3] = static_cast<std::uint8_t>((val >> 24U) & 0xFFU);
}

inline std::uint32_t read_u32_le(const std::uint8_t* src) noexcept {
    return static_cast<std::uint32_t>(src[0]) |
           (static_cast<std::uint32_t>(src[1]) << 8U) |
           (static_cast<std::uint32_t>(src[2]) << 16U) |
           (static_cast<std::uint32_t>(src[3]) << 24U);
}

inline void write_u64_le(std::uint8_t* dst, std::uint64_t val) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        dst[i] = static_cast<std::uint8_t>((val >> (i * 8U)) & 0xFFU);
    }
}

inline std::uint64_t read_u64_le(const std::uint8_t* src) noexcept {
    std::uint64_t val = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        val |= (static_cast<std::uint64_t>(src[i]) << (i * 8U));
    }
    return val;
}

} // namespace

DirectoryManager::DirectoryManager(filler::storage::BlockDevice& device)
    : device_(device),
      owned_inode_manager_(std::make_unique<filler::fs::InodeManager>(device)),
      owned_block_bitmap_(std::make_unique<filler::fs::BlockBitmap>(device)),
      inode_manager_(*owned_inode_manager_),
      block_bitmap_(*owned_block_bitmap_) {}

DirectoryManager::DirectoryManager(filler::storage::BlockDevice& device,
                                   filler::fs::InodeManager& inode_manager,
                                   filler::fs::BlockBitmap& block_bitmap)
    : device_(device),
      owned_inode_manager_(nullptr),
      owned_block_bitmap_(nullptr),
      inode_manager_(inode_manager),
      block_bitmap_(block_bitmap) {}

std::vector<DirectoryEntry> DirectoryManager::read_entries(const Inode& dir_inode) {
    std::vector<DirectoryEntry> entries;
    std::vector<std::uint8_t> block_buf(BlockDevice::BLOCK_SIZE);
    std::size_t remaining_size = dir_inode.size_bytes;

    for (std::size_t i = 0; i < Inode::DIRECT_BLOCKS_COUNT; ++i) {
        if (remaining_size == 0) {
            break;
        }
        const std::uint64_t blk_id = dir_inode.direct_blocks[i];
        if (blk_id == 0) {
            break;
        }

        device_.read_block(blk_id, block_buf.data(), block_buf.size());
        const std::size_t bytes_in_this_block = std::min(remaining_size, BlockDevice::BLOCK_SIZE);

        std::size_t offset = 0;
        while (offset + 16 <= bytes_in_this_block) {
            const std::uint64_t ino = read_u64_le(block_buf.data() + offset);
            const std::uint32_t type_val = read_u32_le(block_buf.data() + offset + 8);
            const std::uint32_t name_len = read_u32_le(block_buf.data() + offset + 12);

            if (ino == 0 || name_len == 0 || name_len > 255) {
                break;
            }

            const std::size_t rec_len = ((16 + name_len + 3) / 4) * 4;
            if (offset + 16 + name_len > bytes_in_this_block) {
                break;
            }

            std::string name(reinterpret_cast<const char*>(block_buf.data() + offset + 16), name_len);
            entries.emplace_back(ino, static_cast<FileType>(type_val), std::move(name));

            offset += rec_len;
        }

        remaining_size -= bytes_in_this_block;
    }

    return entries;
}

void DirectoryManager::write_entries(Inode& dir_inode, const std::vector<DirectoryEntry>& entries) {
    // Pack all directory entries into continuous bytes
    std::vector<std::uint8_t> packed_data;
    for (const auto& entry : entries) {
        const std::size_t name_len = entry.name.size();
        const std::size_t rec_len = ((16 + name_len + 3) / 4) * 4;

        std::vector<std::uint8_t> record(rec_len, 0);
        write_u64_le(record.data(), entry.inode_id);
        write_u32_le(record.data() + 8, static_cast<std::uint32_t>(entry.type));
        write_u32_le(record.data() + 12, static_cast<std::uint32_t>(name_len));
        std::memcpy(record.data() + 16, entry.name.data(), name_len);

        packed_data.insert(packed_data.end(), record.begin(), record.end());
    }

    const std::size_t total_size = packed_data.size();
    const std::size_t blocks_needed = (total_size == 0) ? 1 : ((total_size + BlockDevice::BLOCK_SIZE - 1) / BlockDevice::BLOCK_SIZE);

    if (blocks_needed > Inode::DIRECT_BLOCKS_COUNT) {
        throw DirectoryManagerError("Directory size exceeds direct blocks capacity");
    }

    // Allocate any additional direct blocks needed
    while (dir_inode.blocks_count < blocks_needed) {
        const std::uint64_t new_blk = block_bitmap_.allocate();
        dir_inode.direct_blocks[dir_inode.blocks_count] = new_blk;
        ++dir_inode.blocks_count;
    }

    // Free any excess blocks if size shrank
    while (dir_inode.blocks_count > blocks_needed && dir_inode.blocks_count > 1) {
        --dir_inode.blocks_count;
        const std::uint64_t excess_blk = dir_inode.direct_blocks[dir_inode.blocks_count];
        dir_inode.direct_blocks[dir_inode.blocks_count] = 0;
        block_bitmap_.free(excess_blk);
    }

    // Persist packed entries block-by-block
    std::vector<std::uint8_t> block_buf(BlockDevice::BLOCK_SIZE, 0);
    std::size_t bytes_written = 0;

    for (std::size_t b = 0; b < dir_inode.blocks_count; ++b) {
        std::fill(block_buf.begin(), block_buf.end(), 0);
        if (bytes_written < total_size) {
            const std::size_t chunk = std::min(total_size - bytes_written, BlockDevice::BLOCK_SIZE);
            std::memcpy(block_buf.data(), packed_data.data() + bytes_written, chunk);
            bytes_written += chunk;
        }
        device_.write_block(dir_inode.direct_blocks[b], block_buf.data(), block_buf.size());
    }

    dir_inode.size_bytes = total_size;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    dir_inode.mtime_sec = now;
    dir_inode.ctime_sec = now;

    inode_manager_.update_inode(dir_inode.inode_id, dir_inode);
}

void DirectoryManager::initialize_root() {
    if (!device_.is_open()) {
        throw DirectoryManagerError("Cannot initialize root: BlockDevice is not open");
    }

    if (!inode_manager_.is_inode_allocated(Superblock::DEFAULT_ROOT_INODE)) {
        throw DirectoryManagerError("Cannot initialize root: root inode 1 is not allocated in inode bitmap");
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();

    Inode root_inode;
    root_inode.inode_id = Superblock::DEFAULT_ROOT_INODE;
    root_inode.type = FileType::DIRECTORY;
    root_inode.permissions = 0755;
    root_inode.uid = 0;
    root_inode.gid = 0;
    root_inode.atime_sec = now;
    root_inode.mtime_sec = now;
    root_inode.ctime_sec = now;

    // Check if root already has an allocated block (reuse to avoid leak)
    std::uint64_t root_block = 0;
    try {
        Inode existing = inode_manager_.get_inode(Superblock::DEFAULT_ROOT_INODE);
        if (existing.is_directory() && existing.blocks_count > 0 && existing.direct_blocks[0] != 0) {
            root_block = existing.direct_blocks[0];
            // Free any excess blocks beyond the first
            for (std::size_t i = 1; i < existing.blocks_count; ++i) {
                if (existing.direct_blocks[i] != 0) {
                    block_bitmap_.free(existing.direct_blocks[i]);
                }
            }
        }
    } catch (...) {
        root_block = 0;
    }

    if (root_block == 0) {
        root_block = block_bitmap_.allocate();
    }

    root_inode.direct_blocks[0] = root_block;
    root_inode.blocks_count = 1;

    // Add . and .. entries pointing to root (inode 1)
    std::vector<DirectoryEntry> entries;
    entries.emplace_back(Superblock::DEFAULT_ROOT_INODE, FileType::DIRECTORY, ".");
    entries.emplace_back(Superblock::DEFAULT_ROOT_INODE, FileType::DIRECTORY, "..");

    write_entries(root_inode, entries);
}

std::uint64_t DirectoryManager::mkdir(std::uint64_t parent_inode_id, const std::string& name) {
    if (!device_.is_open()) {
        throw DirectoryManagerError("Cannot mkdir: BlockDevice is not open");
    }

    if (parent_inode_id >= inode_manager_.superblock().total_inodes) {
        throw DirectoryManagerError("Invalid parent inode ID: " + std::to_string(parent_inode_id));
    }

    if (!inode_manager_.is_inode_allocated(parent_inode_id)) {
        throw DirectoryManagerError("Parent inode is not allocated: " + std::to_string(parent_inode_id));
    }

    Inode parent = inode_manager_.get_inode(parent_inode_id);
    if (!parent.is_directory()) {
        throw DirectoryManagerError("Parent inode " + std::to_string(parent_inode_id) + " is not a directory");
    }

    if (name.empty()) {
        throw DirectoryManagerError("Directory name cannot be empty");
    }
    if (name == "." || name == "..") {
        throw DirectoryManagerError("Cannot mkdir with reserved name '" + name + "'");
    }
    if (name.find('/') != std::string::npos || name.find('\0') != std::string::npos) {
        throw DirectoryManagerError("Directory name contains invalid characters: " + name);
    }
    if (name.size() > 255) {
        throw DirectoryManagerError("Directory name exceeds maximum length of 255 bytes");
    }

    // Check duplicate name
    std::vector<DirectoryEntry> parent_entries = read_entries(parent);
    for (const auto& entry : parent_entries) {
        if (entry.name == name) {
            throw DirectoryManagerError("Directory entry already exists: " + name);
        }
    }

    // Allocate child inode
    const std::uint64_t child_inode_id = inode_manager_.allocate_inode(FileType::DIRECTORY, 0755);

    // Allocate child block
    std::uint64_t child_block = 0;
    try {
        child_block = block_bitmap_.allocate();
    } catch (...) {
        inode_manager_.free_inode(child_inode_id);
        throw;
    }

    Inode child = inode_manager_.get_inode(child_inode_id);
    child.direct_blocks[0] = child_block;
    child.blocks_count = 1;

    // Initialize child . and ..
    std::vector<DirectoryEntry> child_entries;
    child_entries.emplace_back(child_inode_id, FileType::DIRECTORY, ".");
    child_entries.emplace_back(parent_inode_id, FileType::DIRECTORY, "..");

    try {
        write_entries(child, child_entries);
    } catch (...) {
        block_bitmap_.free(child_block);
        inode_manager_.free_inode(child_inode_id);
        throw;
    }

    // Add entry to parent
    parent_entries.emplace_back(child_inode_id, FileType::DIRECTORY, name);
    try {
        write_entries(parent, parent_entries);
    } catch (...) {
        block_bitmap_.free(child_block);
        inode_manager_.free_inode(child_inode_id);
        throw;
    }

    return child_inode_id;
}

std::uint64_t DirectoryManager::lookup(std::uint64_t parent_inode_id, const std::string& name) {
    if (!device_.is_open()) {
        throw DirectoryManagerError("Cannot lookup: BlockDevice is not open");
    }

    if (parent_inode_id >= inode_manager_.superblock().total_inodes) {
        throw DirectoryManagerError("Invalid parent inode ID: " + std::to_string(parent_inode_id));
    }

    if (!inode_manager_.is_inode_allocated(parent_inode_id)) {
        throw DirectoryManagerError("Parent inode is not allocated: " + std::to_string(parent_inode_id));
    }

    Inode parent = inode_manager_.get_inode(parent_inode_id);
    if (!parent.is_directory()) {
        throw DirectoryManagerError("Parent inode " + std::to_string(parent_inode_id) + " is not a directory");
    }

    if (name.empty()) {
        throw DirectoryManagerError("Lookup name cannot be empty");
    }

    std::vector<DirectoryEntry> entries = read_entries(parent);
    for (const auto& entry : entries) {
        if (entry.name == name) {
            return entry.inode_id;
        }
    }

    throw EntryNotFoundError(name);
}

std::vector<DirectoryEntry> DirectoryManager::list(std::uint64_t parent_inode_id) {
    if (!device_.is_open()) {
        throw DirectoryManagerError("Cannot list directory: BlockDevice is not open");
    }

    if (parent_inode_id >= inode_manager_.superblock().total_inodes) {
        throw DirectoryManagerError("Invalid parent inode ID: " + std::to_string(parent_inode_id));
    }

    if (!inode_manager_.is_inode_allocated(parent_inode_id)) {
        throw DirectoryManagerError("Parent inode is not allocated: " + std::to_string(parent_inode_id));
    }

    Inode parent = inode_manager_.get_inode(parent_inode_id);
    if (!parent.is_directory()) {
        throw DirectoryManagerError("Parent inode " + std::to_string(parent_inode_id) + " is not a directory");
    }

    return read_entries(parent);
}

void DirectoryManager::remove_directory(std::uint64_t parent_inode_id, const std::string& name) {
    if (!device_.is_open()) {
        throw DirectoryManagerError("Cannot remove directory: BlockDevice is not open");
    }

    if (name.empty()) {
        throw DirectoryManagerError("Directory name cannot be empty");
    }
    if (name == "." || name == "..") {
        throw DirectoryManagerError("Cannot remove '.' or '..'");
    }

    if (parent_inode_id >= inode_manager_.superblock().total_inodes) {
        throw DirectoryManagerError("Invalid parent inode ID: " + std::to_string(parent_inode_id));
    }

    if (!inode_manager_.is_inode_allocated(parent_inode_id)) {
        throw DirectoryManagerError("Parent inode is not allocated: " + std::to_string(parent_inode_id));
    }

    Inode parent = inode_manager_.get_inode(parent_inode_id);
    if (!parent.is_directory()) {
        throw DirectoryManagerError("Parent inode " + std::to_string(parent_inode_id) + " is not a directory");
    }

    std::vector<DirectoryEntry> parent_entries = read_entries(parent);
    auto it = std::find_if(parent_entries.begin(), parent_entries.end(), [&](const DirectoryEntry& e) {
        return e.name == name;
    });

    if (it == parent_entries.end()) {
        throw EntryNotFoundError(name);
    }

    const std::uint64_t child_inode_id = it->inode_id;

    // Reject removal of root directory
    if (child_inode_id == Superblock::DEFAULT_ROOT_INODE) {
        throw DirectoryManagerError("Cannot remove root directory");
    }

    Inode child = inode_manager_.get_inode(child_inode_id);
    if (!child.is_directory()) {
        throw DirectoryManagerError("Target entry '" + name + "' is not a directory");
    }

    // Reject removal of non-empty directory
    std::vector<DirectoryEntry> child_entries = read_entries(child);
    for (const auto& entry : child_entries) {
        if (entry.name != "." && entry.name != "..") {
            throw DirectoryManagerError("Cannot remove non-empty directory: '" + name + "'");
        }
    }

    // Release child data blocks
    for (std::size_t i = 0; i < Inode::DIRECT_BLOCKS_COUNT; ++i) {
        const std::uint64_t blk = child.direct_blocks[i];
        if (blk != 0) {
            block_bitmap_.free(blk);
            child.direct_blocks[i] = 0;
        }
    }
    child.blocks_count = 0;
    child.size_bytes = 0;

    // Release child inode
    inode_manager_.free_inode(child_inode_id);

    // Remove entry from parent and persist
    parent_entries.erase(it);
    write_entries(parent, parent_entries);
}

} // namespace filler::fs
