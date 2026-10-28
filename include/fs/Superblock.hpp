#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <stdexcept>
#include <string>

namespace filler::storage {
class BlockDevice;
}

namespace filler::fs {

// Custom exception thrown when superblock validation fails.
class SuperblockValidationError : public std::runtime_error {
public:
    explicit SuperblockValidationError(const std::string& message)
        : std::runtime_error("Superblock validation failed: " + message) {}
};

// Custom exception thrown when serialization/deserialization fails.
class SuperblockSerializationError : public std::runtime_error {
public:
    explicit SuperblockSerializationError(const std::string& message)
        : std::runtime_error("Superblock serialization error: " + message) {}
};

namespace offsets {
    inline constexpr std::size_t MAGIC               = 0;
    inline constexpr std::size_t VERSION             = 4;
    inline constexpr std::size_t BLOCK_SIZE          = 8;
    inline constexpr std::size_t INODE_SIZE          = 12;
    inline constexpr std::size_t TOTAL_BLOCKS        = 16;
    inline constexpr std::size_t TOTAL_INODES        = 24;
    inline constexpr std::size_t FREE_BLOCKS         = 32;
    inline constexpr std::size_t FREE_INODES         = 40;
    inline constexpr std::size_t ROOT_INODE          = 48;
    inline constexpr std::size_t BLOCK_BITMAP_START  = 56;
    inline constexpr std::size_t BLOCK_BITMAP_BLOCKS = 64;
    inline constexpr std::size_t INODE_BITMAP_START  = 72;
    inline constexpr std::size_t INODE_BITMAP_BLOCKS = 80;
    inline constexpr std::size_t INODE_TABLE_START   = 88;
    inline constexpr std::size_t INODE_TABLE_BLOCKS  = 96;
    inline constexpr std::size_t DATA_REGION_START   = 104;
    inline constexpr std::size_t DATA_REGION_BLOCKS  = 112;
    inline constexpr std::size_t SERIALIZED_METADATA_SIZE = 120;
} // namespace offsets

struct Superblock {
    // Magic number: "FILL" in ASCII (0x46494C4C)
    static constexpr std::uint32_t MAGIC = 0x46494C4C;
    static constexpr std::uint32_t FORMAT_VERSION = 1;
    static constexpr std::uint32_t DEFAULT_BLOCK_SIZE = 4096;
    static constexpr std::uint32_t DEFAULT_INODE_SIZE = 256;
    static constexpr std::uint64_t DEFAULT_ROOT_INODE = 1;
    static constexpr std::uint64_t RESERVED_INODE = 0;
    static constexpr std::uint64_t SUPERBLOCK_BLOCK_ID = 0;

    // Logical fields
    std::uint32_t magic{MAGIC};
    std::uint32_t version{FORMAT_VERSION};
    std::uint32_t block_size{DEFAULT_BLOCK_SIZE};
    std::uint32_t inode_size{DEFAULT_INODE_SIZE};
    std::uint64_t total_blocks{0};
    std::uint64_t total_inodes{0};
    std::uint64_t free_blocks{0};
    std::uint64_t free_inodes{0};
    std::uint64_t root_inode{DEFAULT_ROOT_INODE};
    std::uint64_t block_bitmap_start{0};
    std::uint64_t block_bitmap_blocks{0};
    std::uint64_t inode_bitmap_start{0};
    std::uint64_t inode_bitmap_blocks{0};
    std::uint64_t inode_table_start{0};
    std::uint64_t inode_table_blocks{0};
    std::uint64_t data_region_start{0};
    std::uint64_t data_region_blocks{0};

    Superblock() = default;

    // Factory method for standard filesystem layout with given total blocks and inodes.
    static Superblock create_default(std::uint64_t total_blocks, std::uint64_t total_inodes = 256);

    // Serialization & Deserialization (explicit little-endian format)
    void serialize(void* buffer, std::size_t buffer_size) const;
    static Superblock deserialize(const void* buffer, std::size_t buffer_size, bool validate = true);

    // Invariant validation
    void validate() const;

    // BlockDevice integration
    static Superblock read(filler::storage::BlockDevice& device, bool check_device_size = true);
    void write(filler::storage::BlockDevice& device, bool check_device_size = true) const;

    // Equality operators
    bool operator==(const Superblock& other) const noexcept;
    bool operator!=(const Superblock& other) const noexcept;
};

} // namespace filler::fs
