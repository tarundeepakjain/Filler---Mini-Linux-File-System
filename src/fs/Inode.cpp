#include "fs/Inode.hpp"

#include <cstring>
#include <string>

namespace filler::fs {

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

void Inode::serialize(void* buffer, std::size_t buffer_size) const {
    if (buffer == nullptr) {
        throw InodeSerializationError("Cannot serialize inode: buffer pointer is null");
    }

    if (buffer_size < INODE_SIZE) {
        throw InodeSerializationError("Cannot serialize inode: buffer size (" +
                                      std::to_string(buffer_size) + ") is smaller than INODE_SIZE (" +
                                      std::to_string(INODE_SIZE) + ")");
    }

    std::memset(buffer, 0, buffer_size);
    auto* dst = static_cast<std::uint8_t*>(buffer);

    write_u64_le(dst + inode_offsets::INODE_ID, inode_id);
    write_u32_le(dst + inode_offsets::TYPE, static_cast<std::uint32_t>(type));
    write_u32_le(dst + inode_offsets::PERMISSIONS, permissions);
    write_u32_le(dst + inode_offsets::UID, uid);
    write_u32_le(dst + inode_offsets::GID, gid);
    write_u64_le(dst + inode_offsets::SIZE_BYTES, size_bytes);
    write_u64_le(dst + inode_offsets::BLOCKS_COUNT, blocks_count);
    write_u64_le(dst + inode_offsets::ATIME_SEC, static_cast<std::uint64_t>(atime_sec));
    write_u64_le(dst + inode_offsets::MTIME_SEC, static_cast<std::uint64_t>(mtime_sec));
    write_u64_le(dst + inode_offsets::CTIME_SEC, static_cast<std::uint64_t>(ctime_sec));

    for (std::size_t i = 0; i < DIRECT_BLOCKS_COUNT; ++i) {
        write_u64_le(dst + inode_offsets::DIRECT_BLOCKS + (i * 8), direct_blocks[i]);
    }

    write_u64_le(dst + inode_offsets::SINGLE_INDIRECT, single_indirect);
    write_u64_le(dst + inode_offsets::DOUBLE_INDIRECT, double_indirect);

    std::memcpy(dst + inode_offsets::RESERVED, reserved.data(), reserved.size());
}

Inode Inode::deserialize(const void* buffer, std::size_t buffer_size) {
    if (buffer == nullptr) {
        throw InodeSerializationError("Cannot deserialize inode: buffer pointer is null");
    }

    if (buffer_size < INODE_SIZE) {
        throw InodeSerializationError("Cannot deserialize inode: buffer size (" +
                                      std::to_string(buffer_size) + ") is smaller than INODE_SIZE (" +
                                      std::to_string(INODE_SIZE) + ")");
    }

    const auto* src = static_cast<const std::uint8_t*>(buffer);
    Inode inode;

    inode.inode_id = read_u64_le(src + inode_offsets::INODE_ID);
    inode.type = static_cast<FileType>(read_u32_le(src + inode_offsets::TYPE));
    inode.permissions = read_u32_le(src + inode_offsets::PERMISSIONS);
    inode.uid = read_u32_le(src + inode_offsets::UID);
    inode.gid = read_u32_le(src + inode_offsets::GID);
    inode.size_bytes = read_u64_le(src + inode_offsets::SIZE_BYTES);
    inode.blocks_count = read_u64_le(src + inode_offsets::BLOCKS_COUNT);
    inode.atime_sec = static_cast<std::int64_t>(read_u64_le(src + inode_offsets::ATIME_SEC));
    inode.mtime_sec = static_cast<std::int64_t>(read_u64_le(src + inode_offsets::MTIME_SEC));
    inode.ctime_sec = static_cast<std::int64_t>(read_u64_le(src + inode_offsets::CTIME_SEC));

    for (std::size_t i = 0; i < DIRECT_BLOCKS_COUNT; ++i) {
        inode.direct_blocks[i] = read_u64_le(src + inode_offsets::DIRECT_BLOCKS + (i * 8));
    }

    inode.single_indirect = read_u64_le(src + inode_offsets::SINGLE_INDIRECT);
    inode.double_indirect = read_u64_le(src + inode_offsets::DOUBLE_INDIRECT);

    std::memcpy(inode.reserved.data(), src + inode_offsets::RESERVED, inode.reserved.size());

    return inode;
}

bool Inode::operator==(const Inode& other) const noexcept {
    return inode_id == other.inode_id &&
           type == other.type &&
           permissions == other.permissions &&
           uid == other.uid &&
           gid == other.gid &&
           size_bytes == other.size_bytes &&
           blocks_count == other.blocks_count &&
           atime_sec == other.atime_sec &&
           mtime_sec == other.mtime_sec &&
           ctime_sec == other.ctime_sec &&
           direct_blocks == other.direct_blocks &&
           single_indirect == other.single_indirect &&
           double_indirect == other.double_indirect &&
           reserved == other.reserved;
}

bool Inode::operator!=(const Inode& other) const noexcept {
    return !(*this == other);
}

} // namespace filler::fs

