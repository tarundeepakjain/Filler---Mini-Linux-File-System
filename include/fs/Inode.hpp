#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace filler::fs {

// Custom exception thrown when inode serialization/deserialization fails.
class InodeSerializationError : public std::runtime_error {
public:
    explicit InodeSerializationError(const std::string& message)
        : std::runtime_error("Inode serialization error: " + message) {}
};

enum class FileType : std::uint32_t {
    UNKNOWN = 0,
    REGULAR = 1,
    DIRECTORY = 2
};

namespace inode_offsets {
    inline constexpr std::size_t INODE_ID         = 0;   // uint64_t (8 bytes)
    inline constexpr std::size_t TYPE             = 8;   // uint32_t (4 bytes)
    inline constexpr std::size_t PERMISSIONS      = 12;  // uint32_t (4 bytes)
    inline constexpr std::size_t UID              = 16;  // uint32_t (4 bytes)
    inline constexpr std::size_t GID              = 20;  // uint32_t (4 bytes)
    inline constexpr std::size_t SIZE_BYTES       = 24;  // uint64_t (8 bytes)
    inline constexpr std::size_t BLOCKS_COUNT     = 32;  // uint64_t (8 bytes)
    inline constexpr std::size_t ATIME_SEC        = 40;  // int64_t  (8 bytes)
    inline constexpr std::size_t MTIME_SEC        = 48;  // int64_t  (8 bytes)
    inline constexpr std::size_t CTIME_SEC        = 56;  // int64_t  (8 bytes)
    inline constexpr std::size_t DIRECT_BLOCKS    = 64;  // 16 * 8 = 128 bytes (offsets 64..191)
    inline constexpr std::size_t SINGLE_INDIRECT  = 192; // uint64_t (8 bytes)
    inline constexpr std::size_t DOUBLE_INDIRECT  = 200; // uint64_t (8 bytes)
    inline constexpr std::size_t RESERVED         = 208; // 48 bytes (offsets 208..255)
    inline constexpr std::size_t SERIALIZED_SIZE  = 256;
} // namespace inode_offsets

struct Inode {
    static constexpr std::size_t INODE_SIZE = 256;
    static constexpr std::size_t DIRECT_BLOCKS_COUNT = 16;

    std::uint64_t inode_id{0};
    FileType type{FileType::UNKNOWN};
    std::uint32_t permissions{0644};
    std::uint32_t uid{0};
    std::uint32_t gid{0};
    std::uint64_t size_bytes{0};
    std::uint64_t blocks_count{0};
    std::int64_t atime_sec{0};
    std::int64_t mtime_sec{0};
    std::int64_t ctime_sec{0};
    std::array<std::uint64_t, DIRECT_BLOCKS_COUNT> direct_blocks{};
    std::uint64_t single_indirect{0};
    std::uint64_t double_indirect{0};
    std::array<std::uint8_t, 48> reserved{};

    Inode() = default;

    // Explicit little-endian serialization & deserialization (256 bytes)
    void serialize(void* buffer, std::size_t buffer_size) const;
    static Inode deserialize(const void* buffer, std::size_t buffer_size);

    [[nodiscard]] bool is_directory() const noexcept {
        return type == FileType::DIRECTORY;
    }

    [[nodiscard]] bool is_regular_file() const noexcept {
        return type == FileType::REGULAR;
    }

    bool operator==(const Inode& other) const noexcept;
    bool operator!=(const Inode& other) const noexcept;
};

} // namespace filler::fs

