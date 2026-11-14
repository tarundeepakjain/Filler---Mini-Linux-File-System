#include "fs/FileManager.hpp"
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

FileManager::FileManager(filler::storage::BlockDevice& device)
    : device_(device),
      owned_inode_manager_(std::make_unique<filler::fs::InodeManager>(device)),
      owned_block_bitmap_(std::make_unique<filler::fs::BlockBitmap>(device)),
      owned_directory_manager_(std::make_unique<filler::fs::DirectoryManager>(device, *owned_inode_manager_, *owned_block_bitmap_)),
      inode_manager_(*owned_inode_manager_),
      block_bitmap_(*owned_block_bitmap_),
      directory_manager_(*owned_directory_manager_) {}

FileManager::FileManager(filler::storage::BlockDevice& device,
                         filler::fs::InodeManager& inode_manager,
                         filler::fs::BlockBitmap& block_bitmap,
                         filler::fs::DirectoryManager& directory_manager)
    : device_(device),
      owned_inode_manager_(nullptr),
      owned_block_bitmap_(nullptr),
      owned_directory_manager_(nullptr),
      inode_manager_(inode_manager),
      block_bitmap_(block_bitmap),
      directory_manager_(directory_manager) {}

std::uint64_t FileManager::get_or_allocate_block(Inode& inode, std::uint64_t logical_block, bool allocate,
                                                std::vector<std::uint64_t>* newly_allocated) {
    constexpr std::uint64_t DIRECT_BLOCKS = Inode::DIRECT_BLOCKS_COUNT;
    constexpr std::uint64_t POINTERS_PER_BLOCK = BlockDevice::BLOCK_SIZE / sizeof(std::uint64_t);
    constexpr std::uint64_t SINGLE_INDIRECT_LIMIT = DIRECT_BLOCKS + POINTERS_PER_BLOCK;
    constexpr std::uint64_t DOUBLE_INDIRECT_LIMIT = SINGLE_INDIRECT_LIMIT + (POINTERS_PER_BLOCK * POINTERS_PER_BLOCK);

    if (logical_block < DIRECT_BLOCKS) {
        if (inode.direct_blocks[logical_block] != 0) {
            return inode.direct_blocks[logical_block];
        }
        if (!allocate) {
            return 0;
        }
        const std::uint64_t new_blk = block_bitmap_.allocate();
        if (newly_allocated != nullptr) {
            newly_allocated->push_back(new_blk);
        }
        try {
            alignas(8) std::uint8_t zero_buf[BlockDevice::BLOCK_SIZE] = {0};
            device_.write_block(new_blk, zero_buf, sizeof(zero_buf));
        } catch (...) {
            if (newly_allocated == nullptr) {
                block_bitmap_.free(new_blk);
            }
            throw;
        }
        inode.direct_blocks[logical_block] = new_blk;
        ++inode.blocks_count;
        return new_blk;
    }

    if (logical_block < SINGLE_INDIRECT_LIMIT) {
        const std::uint64_t index = logical_block - DIRECT_BLOCKS;
        bool newly_allocated_table = false;
        if (inode.single_indirect == 0) {
            if (!allocate) {
                return 0;
            }
            const std::uint64_t indir_blk = block_bitmap_.allocate();
            if (newly_allocated != nullptr) {
                newly_allocated->push_back(indir_blk);
            }
            try {
                alignas(8) std::uint8_t zero_buf[BlockDevice::BLOCK_SIZE] = {0};
                device_.write_block(indir_blk, zero_buf, sizeof(zero_buf));
            } catch (...) {
                if (newly_allocated == nullptr) {
                    block_bitmap_.free(indir_blk);
                }
                throw;
            }
            inode.single_indirect = indir_blk;
            ++inode.blocks_count;
            newly_allocated_table = true;
        }

        alignas(8) std::uint8_t table_buf[BlockDevice::BLOCK_SIZE];
        device_.read_block(inode.single_indirect, table_buf, sizeof(table_buf));
        const std::uint64_t existing_blk = read_u64_le(table_buf + (index * 8));
        if (existing_blk != 0) {
            return existing_blk;
        }
        if (!allocate) {
            return 0;
        }

        std::uint64_t new_blk = 0;
        try {
            new_blk = block_bitmap_.allocate();
        } catch (...) {
            if (newly_allocated_table && newly_allocated == nullptr) {
                block_bitmap_.free(inode.single_indirect);
                inode.single_indirect = 0;
                if (inode.blocks_count > 0) --inode.blocks_count;
            }
            throw;
        }
        if (newly_allocated != nullptr) {
            newly_allocated->push_back(new_blk);
        }

        try {
            alignas(8) std::uint8_t zero_buf[BlockDevice::BLOCK_SIZE] = {0};
            device_.write_block(new_blk, zero_buf, sizeof(zero_buf));

            write_u64_le(table_buf + (index * 8), new_blk);
            device_.write_block(inode.single_indirect, table_buf, sizeof(table_buf));
        } catch (...) {
            if (newly_allocated == nullptr) {
                block_bitmap_.free(new_blk);
                if (newly_allocated_table) {
                    block_bitmap_.free(inode.single_indirect);
                    inode.single_indirect = 0;
                    if (inode.blocks_count > 0) --inode.blocks_count;
                }
            }
            throw;
        }

        ++inode.blocks_count;
        return new_blk;
    }

    if (logical_block < DOUBLE_INDIRECT_LIMIT) {
        const std::uint64_t rel = logical_block - SINGLE_INDIRECT_LIMIT;
        const std::uint64_t l1_idx = rel / POINTERS_PER_BLOCK;
        const std::uint64_t l2_idx = rel % POINTERS_PER_BLOCK;

        bool newly_allocated_l1 = false;
        if (inode.double_indirect == 0) {
            if (!allocate) {
                return 0;
            }
            const std::uint64_t dbl_blk = block_bitmap_.allocate();
            if (newly_allocated != nullptr) {
                newly_allocated->push_back(dbl_blk);
            }
            try {
                alignas(8) std::uint8_t zero_buf[BlockDevice::BLOCK_SIZE] = {0};
                device_.write_block(dbl_blk, zero_buf, sizeof(zero_buf));
            } catch (...) {
                if (newly_allocated == nullptr) {
                    block_bitmap_.free(dbl_blk);
                }
                throw;
            }
            inode.double_indirect = dbl_blk;
            ++inode.blocks_count;
            newly_allocated_l1 = true;
        }

        alignas(8) std::uint8_t l1_buf[BlockDevice::BLOCK_SIZE];
        device_.read_block(inode.double_indirect, l1_buf, sizeof(l1_buf));
        std::uint64_t l2_blk = read_u64_le(l1_buf + (l1_idx * 8));

        bool newly_allocated_l2 = false;
        if (l2_blk == 0) {
            if (!allocate) {
                return 0;
            }
            try {
                l2_blk = block_bitmap_.allocate();
            } catch (...) {
                if (newly_allocated_l1 && newly_allocated == nullptr) {
                    block_bitmap_.free(inode.double_indirect);
                    inode.double_indirect = 0;
                    if (inode.blocks_count > 0) --inode.blocks_count;
                }
                throw;
            }
            if (newly_allocated != nullptr) {
                newly_allocated->push_back(l2_blk);
            }

            try {
                alignas(8) std::uint8_t zero_buf[BlockDevice::BLOCK_SIZE] = {0};
                device_.write_block(l2_blk, zero_buf, sizeof(zero_buf));
                write_u64_le(l1_buf + (l1_idx * 8), l2_blk);
                device_.write_block(inode.double_indirect, l1_buf, sizeof(l1_buf));
            } catch (...) {
                if (newly_allocated == nullptr) {
                    block_bitmap_.free(l2_blk);
                    if (newly_allocated_l1) {
                        block_bitmap_.free(inode.double_indirect);
                        inode.double_indirect = 0;
                        if (inode.blocks_count > 0) --inode.blocks_count;
                    }
                }
                throw;
            }

            ++inode.blocks_count;
            newly_allocated_l2 = true;
        }

        alignas(8) std::uint8_t l2_buf[BlockDevice::BLOCK_SIZE];
        device_.read_block(l2_blk, l2_buf, sizeof(l2_buf));
        const std::uint64_t existing_blk = read_u64_le(l2_buf + (l2_idx * 8));
        if (existing_blk != 0) {
            return existing_blk;
        }
        if (!allocate) {
            return 0;
        }

        std::uint64_t new_blk = 0;
        try {
            new_blk = block_bitmap_.allocate();
        } catch (...) {
            if (newly_allocated == nullptr) {
                if (newly_allocated_l2) {
                    block_bitmap_.free(l2_blk);
                    write_u64_le(l1_buf + (l1_idx * 8), 0);
                    device_.write_block(inode.double_indirect, l1_buf, sizeof(l1_buf));
                    if (inode.blocks_count > 0) --inode.blocks_count;
                }
                if (newly_allocated_l1) {
                    block_bitmap_.free(inode.double_indirect);
                    inode.double_indirect = 0;
                    if (inode.blocks_count > 0) --inode.blocks_count;
                }
            }
            throw;
        }
        if (newly_allocated != nullptr) {
            newly_allocated->push_back(new_blk);
        }

        try {
            alignas(8) std::uint8_t zero_buf[BlockDevice::BLOCK_SIZE] = {0};
            device_.write_block(new_blk, zero_buf, sizeof(zero_buf));

            write_u64_le(l2_buf + (l2_idx * 8), new_blk);
            device_.write_block(l2_blk, l2_buf, sizeof(l2_buf));
        } catch (...) {
            if (newly_allocated == nullptr) {
                block_bitmap_.free(new_blk);
                if (newly_allocated_l2) {
                    block_bitmap_.free(l2_blk);
                    write_u64_le(l1_buf + (l1_idx * 8), 0);
                    device_.write_block(inode.double_indirect, l1_buf, sizeof(l1_buf));
                    if (inode.blocks_count > 0) --inode.blocks_count;
                }
                if (newly_allocated_l1) {
                    block_bitmap_.free(inode.double_indirect);
                    inode.double_indirect = 0;
                    if (inode.blocks_count > 0) --inode.blocks_count;
                }
            }
            throw;
        }

        ++inode.blocks_count;
        return new_blk;
    }

    throw FileManagerError("Logical block index " + std::to_string(logical_block) + " exceeds maximum capacity");
}

void FileManager::free_logical_block(Inode& inode, std::uint64_t logical_block) {
    constexpr std::uint64_t DIRECT_BLOCKS = Inode::DIRECT_BLOCKS_COUNT;
    constexpr std::uint64_t POINTERS_PER_BLOCK = BlockDevice::BLOCK_SIZE / sizeof(std::uint64_t);
    constexpr std::uint64_t SINGLE_INDIRECT_LIMIT = DIRECT_BLOCKS + POINTERS_PER_BLOCK;
    constexpr std::uint64_t DOUBLE_INDIRECT_LIMIT = SINGLE_INDIRECT_LIMIT + (POINTERS_PER_BLOCK * POINTERS_PER_BLOCK);

    if (logical_block < DIRECT_BLOCKS) {
        if (inode.direct_blocks[logical_block] != 0) {
            block_bitmap_.free(inode.direct_blocks[logical_block]);
            inode.direct_blocks[logical_block] = 0;
            if (inode.blocks_count > 0) --inode.blocks_count;
        }
        return;
    }

    if (logical_block < SINGLE_INDIRECT_LIMIT) {
        if (inode.single_indirect == 0) return;
        const std::uint64_t index = logical_block - DIRECT_BLOCKS;
        alignas(8) std::uint8_t table_buf[BlockDevice::BLOCK_SIZE];
        device_.read_block(inode.single_indirect, table_buf, sizeof(table_buf));
        const std::uint64_t blk = read_u64_le(table_buf + (index * 8));
        if (blk != 0) {
            block_bitmap_.free(blk);
            write_u64_le(table_buf + (index * 8), 0);
            if (inode.blocks_count > 0) --inode.blocks_count;
            device_.write_block(inode.single_indirect, table_buf, sizeof(table_buf));
        }

        bool any_used = false;
        for (std::uint64_t i = 0; i < POINTERS_PER_BLOCK; ++i) {
            if (read_u64_le(table_buf + (i * 8)) != 0) {
                any_used = true;
                break;
            }
        }
        if (!any_used) {
            block_bitmap_.free(inode.single_indirect);
            inode.single_indirect = 0;
            if (inode.blocks_count > 0) --inode.blocks_count;
        }
        return;
    }

    if (logical_block < DOUBLE_INDIRECT_LIMIT) {
        if (inode.double_indirect == 0) return;
        const std::uint64_t rel = logical_block - SINGLE_INDIRECT_LIMIT;
        const std::uint64_t l1_idx = rel / POINTERS_PER_BLOCK;
        const std::uint64_t l2_idx = rel % POINTERS_PER_BLOCK;

        alignas(8) std::uint8_t l1_buf[BlockDevice::BLOCK_SIZE];
        device_.read_block(inode.double_indirect, l1_buf, sizeof(l1_buf));
        const std::uint64_t l2_blk = read_u64_le(l1_buf + (l1_idx * 8));
        if (l2_blk == 0) return;

        alignas(8) std::uint8_t l2_buf[BlockDevice::BLOCK_SIZE];
        device_.read_block(l2_blk, l2_buf, sizeof(l2_buf));
        const std::uint64_t blk = read_u64_le(l2_buf + (l2_idx * 8));
        if (blk != 0) {
            block_bitmap_.free(blk);
            write_u64_le(l2_buf + (l2_idx * 8), 0);
            if (inode.blocks_count > 0) --inode.blocks_count;
            device_.write_block(l2_blk, l2_buf, sizeof(l2_buf));
        }

        bool l2_used = false;
        for (std::uint64_t j = 0; j < POINTERS_PER_BLOCK; ++j) {
            if (read_u64_le(l2_buf + (j * 8)) != 0) {
                l2_used = true;
                break;
            }
        }
        if (!l2_used) {
            block_bitmap_.free(l2_blk);
            write_u64_le(l1_buf + (l1_idx * 8), 0);
            if (inode.blocks_count > 0) --inode.blocks_count;
            device_.write_block(inode.double_indirect, l1_buf, sizeof(l1_buf));
        }

        bool l1_used = false;
        for (std::uint64_t i = 0; i < POINTERS_PER_BLOCK; ++i) {
            if (read_u64_le(l1_buf + (i * 8)) != 0) {
                l1_used = true;
                break;
            }
        }
        if (!l1_used) {
            block_bitmap_.free(inode.double_indirect);
            inode.double_indirect = 0;
            if (inode.blocks_count > 0) --inode.blocks_count;
        }
    }
}

void FileManager::free_all_blocks(Inode& inode) {
    constexpr std::uint64_t POINTERS_PER_BLOCK = BlockDevice::BLOCK_SIZE / sizeof(std::uint64_t);

    for (std::size_t i = 0; i < Inode::DIRECT_BLOCKS_COUNT; ++i) {
        if (inode.direct_blocks[i] != 0) {
            block_bitmap_.free(inode.direct_blocks[i]);
            inode.direct_blocks[i] = 0;
        }
    }

    if (inode.single_indirect != 0) {
        alignas(8) std::uint8_t table_buf[BlockDevice::BLOCK_SIZE];
        device_.read_block(inode.single_indirect, table_buf, sizeof(table_buf));
        for (std::uint64_t i = 0; i < POINTERS_PER_BLOCK; ++i) {
            const std::uint64_t blk = read_u64_le(table_buf + (i * 8));
            if (blk != 0) {
                block_bitmap_.free(blk);
            }
        }
        block_bitmap_.free(inode.single_indirect);
        inode.single_indirect = 0;
    }

    if (inode.double_indirect != 0) {
        alignas(8) std::uint8_t l1_buf[BlockDevice::BLOCK_SIZE];
        device_.read_block(inode.double_indirect, l1_buf, sizeof(l1_buf));
        for (std::uint64_t i = 0; i < POINTERS_PER_BLOCK; ++i) {
            const std::uint64_t l2_blk = read_u64_le(l1_buf + (i * 8));
            if (l2_blk != 0) {
                alignas(8) std::uint8_t l2_buf[BlockDevice::BLOCK_SIZE];
                device_.read_block(l2_blk, l2_buf, sizeof(l2_buf));
                for (std::uint64_t j = 0; j < POINTERS_PER_BLOCK; ++j) {
                    const std::uint64_t blk = read_u64_le(l2_buf + (j * 8));
                    if (blk != 0) {
                        block_bitmap_.free(blk);
                    }
                }
                block_bitmap_.free(l2_blk);
            }
        }
        block_bitmap_.free(inode.double_indirect);
        inode.double_indirect = 0;
    }

    inode.blocks_count = 0;
    inode.size_bytes = 0;
}

std::uint64_t FileManager::create(std::uint64_t parent_inode_id, const std::string& filename, std::uint32_t permissions) {
    if (!device_.is_open()) {
        throw FileManagerError("Cannot create file: BlockDevice is not open");
    }

    if (filename.empty()) {
        throw FileManagerError("Filename cannot be empty");
    }
    if (filename == "." || filename == "..") {
        throw FileManagerError("Cannot create file with reserved name '" + filename + "'");
    }
    if (filename.find('/') != std::string::npos || filename.find('\0') != std::string::npos) {
        throw FileManagerError("Filename contains invalid characters: " + filename);
    }
    if (filename.size() > 255) {
        throw FileManagerError("Filename exceeds maximum length of 255 bytes");
    }

    if (parent_inode_id >= inode_manager_.superblock().total_inodes) {
        throw FileManagerError("Invalid parent inode ID: " + std::to_string(parent_inode_id));
    }

    if (!inode_manager_.is_inode_allocated(parent_inode_id)) {
        throw FileManagerError("Parent inode is not allocated: " + std::to_string(parent_inode_id));
    }

    Inode parent = inode_manager_.get_inode(parent_inode_id);
    if (!parent.is_directory()) {
        throw FileManagerError("Parent inode " + std::to_string(parent_inode_id) + " is not a directory");
    }

    // Allocate inode for regular file
    const std::uint64_t file_inode_id = inode_manager_.allocate_inode(FileType::REGULAR, permissions);

    try {
        directory_manager_.add_entry(parent_inode_id, file_inode_id, FileType::REGULAR, filename);
    } catch (const DirectoryManagerError& ex) {
        inode_manager_.free_inode(file_inode_id);
        throw FileManagerError(ex.what());
    } catch (...) {
        inode_manager_.free_inode(file_inode_id);
        throw;
    }

    return file_inode_id;
}

std::size_t FileManager::read(std::uint64_t inode_id, std::uint64_t offset, void* buffer, std::size_t size) {
    if (!device_.is_open()) {
        throw FileManagerError("Cannot read file: BlockDevice is not open");
    }

    if (size == 0) {
        return 0;
    }

    if (buffer == nullptr) {
        throw FileManagerError("Cannot read file: buffer pointer is null");
    }

    if (inode_id >= inode_manager_.superblock().total_inodes) {
        throw FileManagerError("Invalid inode ID: " + std::to_string(inode_id));
    }

    if (!inode_manager_.is_inode_allocated(inode_id)) {
        throw FileManagerError("Inode " + std::to_string(inode_id) + " is not allocated");
    }

    Inode inode = inode_manager_.get_inode(inode_id);
    if (inode.is_directory()) {
        throw FileManagerError("Cannot read from a directory with FileManager::read (inode " + std::to_string(inode_id) + ")");
    }

    if (offset >= inode.size_bytes) {
        return 0; // EOF reached
    }

    const std::size_t to_read = std::min(size, static_cast<std::size_t>(inode.size_bytes - offset));
    auto* dest = static_cast<std::uint8_t*>(buffer);
    std::size_t bytes_read = 0;
    alignas(8) std::uint8_t block_buf[BlockDevice::BLOCK_SIZE];

    while (bytes_read < to_read) {
        const std::uint64_t current_offset = offset + bytes_read;
        const std::uint64_t logical_block = current_offset / BlockDevice::BLOCK_SIZE;
        const std::size_t block_offset = current_offset % BlockDevice::BLOCK_SIZE;
        const std::size_t chunk = std::min(to_read - bytes_read, BlockDevice::BLOCK_SIZE - block_offset);

        const std::uint64_t phys_block = get_or_allocate_block(inode, logical_block, false);
        if (phys_block == 0) {
            std::memset(dest + bytes_read, 0, chunk);
        } else {
            device_.read_block(phys_block, block_buf, sizeof(block_buf));
            std::memcpy(dest + bytes_read, block_buf + block_offset, chunk);
        }

        bytes_read += chunk;
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    inode.atime_sec = now;
    inode_manager_.update_inode(inode_id, inode);

    return bytes_read;
}

std::vector<std::uint8_t> FileManager::read(std::uint64_t inode_id, std::uint64_t offset, std::size_t size) {
    std::vector<std::uint8_t> buffer(size);
    const std::size_t actual_read = read(inode_id, offset, buffer.data(), size);
    buffer.resize(actual_read);
    return buffer;
}

std::size_t FileManager::write(std::uint64_t inode_id, std::uint64_t offset, const void* data, std::size_t size) {
    if (!device_.is_open()) {
        throw FileManagerError("Cannot write file: BlockDevice is not open");
    }

    if (size == 0) {
        return 0;
    }

    if (data == nullptr) {
        throw FileManagerError("Cannot write file: data pointer is null");
    }

    if (inode_id >= inode_manager_.superblock().total_inodes) {
        throw FileManagerError("Invalid inode ID: " + std::to_string(inode_id));
    }

    if (!inode_manager_.is_inode_allocated(inode_id)) {
        throw FileManagerError("Inode " + std::to_string(inode_id) + " is not allocated");
    }

    Inode inode = inode_manager_.get_inode(inode_id);
    if (inode.is_directory()) {
        throw FileManagerError("Cannot write to a directory with FileManager::write (inode " + std::to_string(inode_id) + ")");
    }

    const auto* src = static_cast<const std::uint8_t*>(data);
    std::size_t bytes_written = 0;
    alignas(8) std::uint8_t block_buf[BlockDevice::BLOCK_SIZE];

    try {
        while (bytes_written < size) {
            const std::uint64_t current_offset = offset + bytes_written;
            const std::uint64_t logical_block = current_offset / BlockDevice::BLOCK_SIZE;
            const std::size_t block_offset = current_offset % BlockDevice::BLOCK_SIZE;
            const std::size_t chunk = std::min(size - bytes_written, BlockDevice::BLOCK_SIZE - block_offset);

            const std::uint64_t phys_block = get_or_allocate_block(inode, logical_block, true);
            if (chunk < BlockDevice::BLOCK_SIZE) {
                device_.read_block(phys_block, block_buf, sizeof(block_buf));
            }

            std::memcpy(block_buf + block_offset, src + bytes_written, chunk);
            device_.write_block(phys_block, block_buf, sizeof(block_buf));

            bytes_written += chunk;
        }
    } catch (...) {
        if (bytes_written > 0) {
            if (offset + bytes_written > inode.size_bytes) {
                inode.size_bytes = offset + bytes_written;
            }
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch()).count();
            inode.mtime_sec = now;
            inode.ctime_sec = now;
            inode_manager_.update_inode(inode_id, inode);
        }
        throw;
    }

    if (offset + size > inode.size_bytes) {
        inode.size_bytes = offset + size;
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    inode.mtime_sec = now;
    inode.ctime_sec = now;
    inode_manager_.update_inode(inode_id, inode);

    return bytes_written;
}

std::size_t FileManager::write(std::uint64_t inode_id, std::uint64_t offset, const std::vector<std::uint8_t>& data) {
    return write(inode_id, offset, data.data(), data.size());
}

std::size_t FileManager::write(std::uint64_t inode_id, std::uint64_t offset, const std::string& data) {
    return write(inode_id, offset, data.data(), data.size());
}

std::size_t FileManager::append(std::uint64_t inode_id, const void* data, std::size_t size) {
    Inode inode = stat(inode_id);
    return write(inode_id, inode.size_bytes, data, size);
}

std::size_t FileManager::append(std::uint64_t inode_id, const std::vector<std::uint8_t>& data) {
    return append(inode_id, data.data(), data.size());
}

std::size_t FileManager::append(std::uint64_t inode_id, const std::string& data) {
    return append(inode_id, data.data(), data.size());
}

void FileManager::truncate(std::uint64_t inode_id, std::uint64_t new_size) {
    if (!device_.is_open()) {
        throw FileManagerError("Cannot truncate file: BlockDevice is not open");
    }

    if (inode_id >= inode_manager_.superblock().total_inodes) {
        throw FileManagerError("Invalid inode ID: " + std::to_string(inode_id));
    }

    if (!inode_manager_.is_inode_allocated(inode_id)) {
        throw FileManagerError("Inode " + std::to_string(inode_id) + " is not allocated");
    }

    Inode inode = inode_manager_.get_inode(inode_id);
    if (inode.is_directory()) {
        throw FileManagerError("Cannot truncate directory (inode " + std::to_string(inode_id) + ")");
    }

    if (new_size == inode.size_bytes) {
        return;
    }

    if (new_size == 0) {
        free_all_blocks(inode);
        inode.size_bytes = 0;
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count();
        inode.mtime_sec = now;
        inode.ctime_sec = now;
        inode_manager_.update_inode(inode_id, inode);
        return;
    }

    if (new_size < inode.size_bytes) {
        const std::uint64_t old_blocks = (inode.size_bytes + BlockDevice::BLOCK_SIZE - 1) / BlockDevice::BLOCK_SIZE;
        const std::uint64_t new_blocks = (new_size + BlockDevice::BLOCK_SIZE - 1) / BlockDevice::BLOCK_SIZE;

        for (std::uint64_t blk = old_blocks; blk > new_blocks; --blk) {
            free_logical_block(inode, blk - 1);
        }

        // Zero out the remaining unused portion in the new final block
        const std::size_t offset_in_last = new_size % BlockDevice::BLOCK_SIZE;
        if (offset_in_last != 0) {
            const std::uint64_t last_logical_block = new_size / BlockDevice::BLOCK_SIZE;
            const std::uint64_t phys_block = get_or_allocate_block(inode, last_logical_block, false);
            if (phys_block != 0) {
                alignas(8) std::uint8_t block_buf[BlockDevice::BLOCK_SIZE];
                device_.read_block(phys_block, block_buf, sizeof(block_buf));
                std::memset(block_buf + offset_in_last, 0, BlockDevice::BLOCK_SIZE - offset_in_last);
                device_.write_block(phys_block, block_buf, sizeof(block_buf));
            }
        }

        inode.size_bytes = new_size;
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count();
        inode.mtime_sec = now;
        inode.ctime_sec = now;
        inode_manager_.update_inode(inode_id, inode);
        return;
    }

    // Growing file:
    // Work on a working copy of inode and track every newly allocated block for rollback.
    Inode working_inode = inode;
    std::vector<std::uint64_t> newly_allocated_blocks;

    try {
        const std::uint64_t old_blocks = (working_inode.size_bytes + BlockDevice::BLOCK_SIZE - 1) / BlockDevice::BLOCK_SIZE;
        const std::uint64_t new_blocks = (new_size + BlockDevice::BLOCK_SIZE - 1) / BlockDevice::BLOCK_SIZE;

        for (std::uint64_t blk = old_blocks; blk < new_blocks; ++blk) {
            get_or_allocate_block(working_inode, blk, true, &newly_allocated_blocks);
        }

        working_inode.size_bytes = new_size;
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count();
        working_inode.mtime_sec = now;
        working_inode.ctime_sec = now;
        inode_manager_.update_inode(inode_id, working_inode);
    } catch (...) {
        // Rollback all newly allocated blocks in reverse order
        for (auto it = newly_allocated_blocks.rbegin(); it != newly_allocated_blocks.rend(); ++it) {
            try {
                block_bitmap_.free(*it);
            } catch (...) {
                // Ignore secondary errors during rollback to preserve primary exception
            }
        }
        // Original inode metadata and file size remain unchanged on disk
        throw;
    }
}

void FileManager::remove(std::uint64_t parent_inode_id, const std::string& filename) {
    if (!device_.is_open()) {
        throw FileManagerError("Cannot remove file: BlockDevice is not open");
    }

    if (filename.empty()) {
        throw FileManagerError("Filename cannot be empty");
    }
    if (filename == "." || filename == "..") {
        throw FileManagerError("Cannot remove '.' or '..'");
    }

    if (parent_inode_id >= inode_manager_.superblock().total_inodes) {
        throw FileManagerError("Invalid parent inode ID: " + std::to_string(parent_inode_id));
    }

    if (!inode_manager_.is_inode_allocated(parent_inode_id)) {
        throw FileManagerError("Parent inode is not allocated: " + std::to_string(parent_inode_id));
    }

    Inode parent = inode_manager_.get_inode(parent_inode_id);
    if (!parent.is_directory()) {
        throw FileManagerError("Parent inode " + std::to_string(parent_inode_id) + " is not a directory");
    }

    // Step 1: Validate target entry exists
    std::uint64_t file_inode_id = 0;
    try {
        file_inode_id = directory_manager_.lookup(parent_inode_id, filename);
    } catch (const EntryNotFoundError&) {
        throw FileNotFoundException(filename);
    } catch (const DirectoryManagerError& ex) {
        throw FileManagerError(ex.what());
    }

    // Step 2: Validate target inode
    if (file_inode_id >= inode_manager_.superblock().total_inodes ||
        !inode_manager_.is_inode_allocated(file_inode_id)) {
        throw FileManagerError("Target file inode " + std::to_string(file_inode_id) + " is invalid or not allocated");
    }

    Inode file_inode = inode_manager_.get_inode(file_inode_id);
    if (file_inode.is_directory()) {
        throw FileManagerError("Cannot remove directory with FileManager::remove (use DirectoryManager::remove_directory)");
    }

    // Step 3: Remove entry from parent directory FIRST.
    // Ordering rationale:
    // By removing the directory entry first, we prevent dangling directory references to freed inodes.
    // If directory entry removal fails, the file blocks and inode remain untouched.
    // Note: In filesystems without a write-ahead journal, a crash between directory entry
    // removal and resource release could leave orphaned blocks/inodes (recoverable by fsck),
    // but prevents filesystem corruption from active directory entries referencing freed inodes.
    try {
        directory_manager_.remove_entry(parent_inode_id, filename);
    } catch (const DirectoryManagerError& ex) {
        throw FileManagerError(ex.what());
    }

    // Step 4: Release all file blocks
    free_all_blocks(file_inode);

    // Step 5: Release file inode
    inode_manager_.free_inode(file_inode_id);
}

Inode FileManager::stat(std::uint64_t inode_id) {
    if (!device_.is_open()) {
        throw FileManagerError("Cannot stat: BlockDevice is not open");
    }

    if (inode_id >= inode_manager_.superblock().total_inodes) {
        throw FileManagerError("Invalid inode ID: " + std::to_string(inode_id));
    }

    if (!inode_manager_.is_inode_allocated(inode_id)) {
        throw FileManagerError("Inode " + std::to_string(inode_id) + " is not allocated");
    }

    return inode_manager_.get_inode(inode_id);
}

} // namespace filler::fs
