#pragma once

#include "fs/BlockBitmap.hpp"
#include "fs/DirectoryManager.hpp"
#include "fs/Inode.hpp"
#include "fs/InodeManager.hpp"
#include "storage/BlockDevice.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace filler::fs {

class FileManagerError : public std::runtime_error {
public:
    explicit FileManagerError(const std::string& message)
        : std::runtime_error("FileManager error: " + message) {}
};

class FileNotFoundException : public FileManagerError {
public:
    explicit FileNotFoundException(const std::string& name)
        : FileManagerError("File not found: " + name) {}
};

class FileManager {
public:
    explicit FileManager(filler::storage::BlockDevice& device);

    FileManager(filler::storage::BlockDevice& device,
                filler::fs::InodeManager& inode_manager,
                filler::fs::BlockBitmap& block_bitmap,
                filler::fs::DirectoryManager& directory_manager);

    // Creates an empty regular file in the specified directory.
    // Returns the allocated inode ID of the new regular file.
    std::uint64_t create(std::uint64_t parent_inode_id, const std::string& filename, std::uint32_t permissions = 0644);

    // Reads up to size bytes from inode_id starting at offset into buffer.
    // Returns the number of bytes read (0 if offset >= EOF).
    std::size_t read(std::uint64_t inode_id, std::uint64_t offset, void* buffer, std::size_t size);

    // Reads up to size bytes from inode_id starting at offset and returns as a byte vector.
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t inode_id, std::uint64_t offset, std::size_t size);

    // Writes size bytes from data to inode_id starting at offset.
    // Allocates blocks as necessary and updates file size. Returns bytes written.
    std::size_t write(std::uint64_t inode_id, std::uint64_t offset, const void* data, std::size_t size);

    // Convenience overload for writing byte vectors.
    std::size_t write(std::uint64_t inode_id, std::uint64_t offset, const std::vector<std::uint8_t>& data);

    // Convenience overload for writing strings.
    std::size_t write(std::uint64_t inode_id, std::uint64_t offset, const std::string& data);

    // Appends size bytes to the end of inode_id. Returns bytes written.
    std::size_t append(std::uint64_t inode_id, const void* data, std::size_t size);

    // Convenience overload for appending byte vectors.
    std::size_t append(std::uint64_t inode_id, const std::vector<std::uint8_t>& data);

    // Convenience overload for appending strings.
    std::size_t append(std::uint64_t inode_id, const std::string& data);

    // Resizes file to new_size, releasing blocks when shrinking and allocating blocks when growing.
    void truncate(std::uint64_t inode_id, std::uint64_t new_size);

    // Removes a regular file from parent_inode_id and releases its blocks and inode.
    void remove(std::uint64_t parent_inode_id, const std::string& filename);

    // Retrieves file metadata using the existing inode representation.
    [[nodiscard]] Inode stat(std::uint64_t inode_id);

    // Accessors
    [[nodiscard]] filler::fs::InodeManager& inode_manager() noexcept { return inode_manager_; }
    [[nodiscard]] filler::fs::BlockBitmap& block_bitmap() noexcept { return block_bitmap_; }
    [[nodiscard]] filler::fs::DirectoryManager& directory_manager() noexcept { return directory_manager_; }
    [[nodiscard]] filler::storage::BlockDevice& device() noexcept { return device_; }

private:
    filler::storage::BlockDevice& device_;
    std::unique_ptr<filler::fs::InodeManager> owned_inode_manager_;
    std::unique_ptr<filler::fs::BlockBitmap> owned_block_bitmap_;
    std::unique_ptr<filler::fs::DirectoryManager> owned_directory_manager_;
    filler::fs::InodeManager& inode_manager_;
    filler::fs::BlockBitmap& block_bitmap_;
    filler::fs::DirectoryManager& directory_manager_;

    std::uint64_t get_or_allocate_block(Inode& inode, std::uint64_t logical_block, bool allocate);
    void free_logical_block(Inode& inode, std::uint64_t logical_block);
    void free_all_blocks(Inode& inode);
};

} // namespace filler::fs
