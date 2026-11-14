#pragma once

#include "fs/BlockBitmap.hpp"
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

struct DirectoryEntry {
    std::uint64_t inode_id{0};
    FileType type{FileType::UNKNOWN};
    std::string name;

    DirectoryEntry() = default;
    DirectoryEntry(std::uint64_t id, FileType t, std::string n)
        : inode_id(id), type(t), name(std::move(n)) {}

    bool operator==(const DirectoryEntry& other) const noexcept {
        return inode_id == other.inode_id &&
               type == other.type &&
               name == other.name;
    }

    bool operator!=(const DirectoryEntry& other) const noexcept {
        return !(*this == other);
    }
};

class DirectoryManagerError : public std::runtime_error {
public:
    explicit DirectoryManagerError(const std::string& message)
        : std::runtime_error("DirectoryManager error: " + message) {}
};

class EntryNotFoundError : public DirectoryManagerError {
public:
    explicit EntryNotFoundError(const std::string& name)
        : DirectoryManagerError("Directory entry not found: " + name) {}
};

class DirectoryManager {
public:
    explicit DirectoryManager(filler::storage::BlockDevice& device);

    DirectoryManager(filler::storage::BlockDevice& device,
                     filler::fs::InodeManager& inode_manager,
                     filler::fs::BlockBitmap& block_bitmap);

    // Initializes root directory (inode 1) with '.' and '..' entries
    void initialize_root();

    // Creates a new directory inside parent_inode_id with the given name.
    // Returns the inode ID of the newly created directory.
    std::uint64_t mkdir(std::uint64_t parent_inode_id, const std::string& name);

    // Searches the specified directory and returns the matching inode number.
    // Throws EntryNotFoundError if the entry does not exist.
    [[nodiscard]] std::uint64_t lookup(std::uint64_t parent_inode_id, const std::string& name);

    // Returns all entries in the specified directory, including '.' and '..'
    [[nodiscard]] std::vector<DirectoryEntry> list(std::uint64_t parent_inode_id);

    // Removes an empty directory from parent_inode_id.
    // Rejects root directory removal and non-empty directory removal.
    void remove_directory(std::uint64_t parent_inode_id, const std::string& name);

    // Adds a directory entry (regular file or directory) to parent_inode_id.
    void add_entry(std::uint64_t parent_inode_id, std::uint64_t inode_id, FileType type, const std::string& name);

    // Removes an entry from parent_inode_id by name.
    void remove_entry(std::uint64_t parent_inode_id, const std::string& name);

    // Accessors
    [[nodiscard]] filler::fs::InodeManager& inode_manager() noexcept { return inode_manager_; }
    [[nodiscard]] filler::fs::BlockBitmap& block_bitmap() noexcept { return block_bitmap_; }
    [[nodiscard]] filler::storage::BlockDevice& device() noexcept { return device_; }

private:
    filler::storage::BlockDevice& device_;
    std::unique_ptr<filler::fs::InodeManager> owned_inode_manager_;
    std::unique_ptr<filler::fs::BlockBitmap> owned_block_bitmap_;
    filler::fs::InodeManager& inode_manager_;
    filler::fs::BlockBitmap& block_bitmap_;

    [[nodiscard]] std::vector<DirectoryEntry> read_entries(const Inode& dir_inode);
    void write_entries(Inode& dir_inode, const std::vector<DirectoryEntry>& entries);
};

} // namespace filler::fs

