#include "fs/Superblock.hpp"
#include "storage/BlockDevice.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
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

inline bool check_add_overflow(std::uint64_t a, std::uint64_t b, std::uint64_t* result = nullptr) noexcept {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) {
        return true;
    }
    if (result != nullptr) {
        *result = a + b;
    }
    return false;
}

} // namespace

Superblock Superblock::create_default(std::uint64_t total_blocks, std::uint64_t total_inodes) {
    if (total_blocks == 0) {
        throw SuperblockValidationError("Total blocks must be greater than 0");
    }
    if (total_inodes < 2) {
        throw SuperblockValidationError("Total inodes must be at least 2 (root inode 1 and reserved inode 0)");
    }

    Superblock sb;
    sb.magic = MAGIC;
    sb.version = FORMAT_VERSION;
    sb.block_size = DEFAULT_BLOCK_SIZE;
    sb.inode_size = DEFAULT_INODE_SIZE;
    sb.total_blocks = total_blocks;
    sb.total_inodes = total_inodes;
    sb.root_inode = DEFAULT_ROOT_INODE;

    // Superblock is block 0 (length 1)
    // Block bitmap starts at block 1
    const std::uint64_t bits_per_block = static_cast<std::uint64_t>(DEFAULT_BLOCK_SIZE) * 8ULL;
    sb.block_bitmap_start = 1;
    sb.block_bitmap_blocks = (total_blocks + bits_per_block - 1ULL) / bits_per_block;

    sb.inode_bitmap_start = sb.block_bitmap_start + sb.block_bitmap_blocks;
    sb.inode_bitmap_blocks = (total_inodes + bits_per_block - 1ULL) / bits_per_block;

    const std::uint64_t inodes_per_block = DEFAULT_BLOCK_SIZE / DEFAULT_INODE_SIZE;
    sb.inode_table_start = sb.inode_bitmap_start + sb.inode_bitmap_blocks;
    sb.inode_table_blocks = (total_inodes + inodes_per_block - 1ULL) / inodes_per_block;

    sb.data_region_start = sb.inode_table_start + sb.inode_table_blocks;
    if (total_blocks <= sb.data_region_start) {
        throw SuperblockValidationError("Total blocks (" + std::to_string(total_blocks) +
                                        ") insufficient for metadata regions (" +
                                        std::to_string(sb.data_region_start) + " blocks required)");
    }
    sb.data_region_blocks = total_blocks - sb.data_region_start;
    sb.free_blocks = sb.data_region_blocks;
    sb.free_inodes = (total_inodes >= 2) ? (total_inodes - 2) : 0;

    sb.validate();
    return sb;
}

void Superblock::validate() const {
    if (magic != MAGIC) {
        throw SuperblockValidationError("Invalid magic number: expected 0x" +
                                        std::to_string(MAGIC) + ", got 0x" + std::to_string(magic));
    }

    if (version != FORMAT_VERSION) {
        throw SuperblockValidationError("Unsupported filesystem version: expected " +
                                        std::to_string(FORMAT_VERSION) + ", got " + std::to_string(version));
    }

    if (block_size != DEFAULT_BLOCK_SIZE) {
        throw SuperblockValidationError("Unsupported block size: expected " +
                                        std::to_string(DEFAULT_BLOCK_SIZE) + ", got " + std::to_string(block_size));
    }

    if (inode_size != DEFAULT_INODE_SIZE) {
        throw SuperblockValidationError("Unsupported inode size: expected " +
                                        std::to_string(DEFAULT_INODE_SIZE) + ", got " + std::to_string(inode_size));
    }

    if (total_blocks == 0) {
        throw SuperblockValidationError("Total blocks must be greater than 0");
    }

    if (total_inodes == 0) {
        throw SuperblockValidationError("Total inodes must be greater than 0");
    }

    if (root_inode == RESERVED_INODE) {
        throw SuperblockValidationError("Root inode cannot be reserved inode 0");
    }

    if (root_inode != DEFAULT_ROOT_INODE) {
        throw SuperblockValidationError("Invalid root inode number: expected " +
                                        std::to_string(DEFAULT_ROOT_INODE) + ", got " + std::to_string(root_inode));
    }

    if (root_inode >= total_inodes) {
        throw SuperblockValidationError("Root inode (" + std::to_string(root_inode) +
                                        ") out of range for total_inodes (" + std::to_string(total_inodes) + ")");
    }

    if (block_bitmap_blocks == 0) {
        throw SuperblockValidationError("Block bitmap length must be at least 1 block");
    }

    if (inode_bitmap_blocks == 0) {
        throw SuperblockValidationError("Inode bitmap length must be at least 1 block");
    }

    if (inode_table_blocks == 0) {
        throw SuperblockValidationError("Inode table length must be at least 1 block");
    }

    if (data_region_blocks == 0) {
        throw SuperblockValidationError("Data region length must be at least 1 block");
    }

    // Check region end boundaries for integer overflow
    std::uint64_t block_bitmap_end = 0;
    if (check_add_overflow(block_bitmap_start, block_bitmap_blocks, &block_bitmap_end)) {
        throw SuperblockValidationError("Block bitmap range overflows 64-bit integer");
    }

    std::uint64_t inode_bitmap_end = 0;
    if (check_add_overflow(inode_bitmap_start, inode_bitmap_blocks, &inode_bitmap_end)) {
        throw SuperblockValidationError("Inode bitmap range overflows 64-bit integer");
    }

    std::uint64_t inode_table_end = 0;
    if (check_add_overflow(inode_table_start, inode_table_blocks, &inode_table_end)) {
        throw SuperblockValidationError("Inode table range overflows 64-bit integer");
    }

    std::uint64_t data_region_end = 0;
    if (check_add_overflow(data_region_start, data_region_blocks, &data_region_end)) {
        throw SuperblockValidationError("Data region range overflows 64-bit integer");
    }

    // Non-overlapping and ordering checks:
    // Block 0 is reserved for Superblock ([0, 1))
    if (block_bitmap_start < 1) {
        throw SuperblockValidationError("Block bitmap starts before or overlaps superblock at block 0");
    }

    if (block_bitmap_end > inode_bitmap_start) {
        throw SuperblockValidationError("Block bitmap region [" + std::to_string(block_bitmap_start) + ", " +
                                        std::to_string(block_bitmap_end) + ") overlaps with inode bitmap start (" +
                                        std::to_string(inode_bitmap_start) + ")");
    }

    if (inode_bitmap_end > inode_table_start) {
        throw SuperblockValidationError("Inode bitmap region [" + std::to_string(inode_bitmap_start) + ", " +
                                        std::to_string(inode_bitmap_end) + ") overlaps with inode table start (" +
                                        std::to_string(inode_table_start) + ")");
    }

    if (inode_table_end > data_region_start) {
        throw SuperblockValidationError("Inode table region [" + std::to_string(inode_table_start) + ", " +
                                        std::to_string(inode_table_end) + ") overlaps with data region start (" +
                                        std::to_string(data_region_start) + ")");
    }

    if (data_region_end > total_blocks) {
        throw SuperblockValidationError("Data region end (" + std::to_string(data_region_end) +
                                        ") extends beyond total blocks (" + std::to_string(total_blocks) + ")");
    }

    // Capacity checks
    const std::uint64_t bits_per_block = static_cast<std::uint64_t>(block_size) * 8ULL;
    const std::uint64_t max_block_bitmap_capacity = (block_bitmap_blocks > std::numeric_limits<std::uint64_t>::max() / bits_per_block)
                                                        ? std::numeric_limits<std::uint64_t>::max()
                                                        : (block_bitmap_blocks * bits_per_block);
    if (total_blocks > max_block_bitmap_capacity) {
        throw SuperblockValidationError("Block bitmap capacity (" + std::to_string(max_block_bitmap_capacity) +
                                        " blocks) is insufficient for total blocks (" +
                                        std::to_string(total_blocks) + ")");
    }

    const std::uint64_t max_inode_bitmap_capacity = (inode_bitmap_blocks > std::numeric_limits<std::uint64_t>::max() / bits_per_block)
                                                        ? std::numeric_limits<std::uint64_t>::max()
                                                        : (inode_bitmap_blocks * bits_per_block);
    if (total_inodes > max_inode_bitmap_capacity) {
        throw SuperblockValidationError("Inode bitmap capacity (" + std::to_string(max_inode_bitmap_capacity) +
                                        " inodes) is insufficient for total inodes (" +
                                        std::to_string(total_inodes) + ")");
    }

    const std::uint64_t inodes_per_block = block_size / inode_size;
    const std::uint64_t max_inode_table_capacity = (inode_table_blocks > std::numeric_limits<std::uint64_t>::max() / inodes_per_block)
                                                       ? std::numeric_limits<std::uint64_t>::max()
                                                       : (inode_table_blocks * inodes_per_block);
    if (total_inodes > max_inode_table_capacity) {
        throw SuperblockValidationError("Inode table capacity (" + std::to_string(max_inode_table_capacity) +
                                        " inodes) is insufficient for total inodes (" +
                                        std::to_string(total_inodes) + ")");
    }

    // Free counts validation
    if (free_blocks > data_region_blocks) {
        throw SuperblockValidationError("Free block count (" + std::to_string(free_blocks) +
                                        ") exceeds data region block count (" +
                                        std::to_string(data_region_blocks) + ")");
    }

    if (free_inodes >= total_inodes) {
        throw SuperblockValidationError("Free inode count (" + std::to_string(free_inodes) +
                                        ") cannot equal or exceed total inodes (" +
                                        std::to_string(total_inodes) + ") because root inode is allocated");
    }
}

void Superblock::serialize(void* buffer, std::size_t buffer_size) const {
    if (buffer == nullptr) {
        throw SuperblockSerializationError("Cannot serialize superblock: buffer pointer is null");
    }

    if (buffer_size < DEFAULT_BLOCK_SIZE) {
        throw SuperblockSerializationError("Cannot serialize superblock: buffer size (" +
                                           std::to_string(buffer_size) + ") is smaller than block size (" +
                                           std::to_string(DEFAULT_BLOCK_SIZE) + ")");
    }

    std::memset(buffer, 0, buffer_size);
    auto* dst = static_cast<std::uint8_t*>(buffer);

    write_u32_le(dst + offsets::MAGIC, magic);
    write_u32_le(dst + offsets::VERSION, version);
    write_u32_le(dst + offsets::BLOCK_SIZE, block_size);
    write_u32_le(dst + offsets::INODE_SIZE, inode_size);
    write_u64_le(dst + offsets::TOTAL_BLOCKS, total_blocks);
    write_u64_le(dst + offsets::TOTAL_INODES, total_inodes);
    write_u64_le(dst + offsets::FREE_BLOCKS, free_blocks);
    write_u64_le(dst + offsets::FREE_INODES, free_inodes);
    write_u64_le(dst + offsets::ROOT_INODE, root_inode);
    write_u64_le(dst + offsets::BLOCK_BITMAP_START, block_bitmap_start);
    write_u64_le(dst + offsets::BLOCK_BITMAP_BLOCKS, block_bitmap_blocks);
    write_u64_le(dst + offsets::INODE_BITMAP_START, inode_bitmap_start);
    write_u64_le(dst + offsets::INODE_BITMAP_BLOCKS, inode_bitmap_blocks);
    write_u64_le(dst + offsets::INODE_TABLE_START, inode_table_start);
    write_u64_le(dst + offsets::INODE_TABLE_BLOCKS, inode_table_blocks);
    write_u64_le(dst + offsets::DATA_REGION_START, data_region_start);
    write_u64_le(dst + offsets::DATA_REGION_BLOCKS, data_region_blocks);
}

Superblock Superblock::deserialize(const void* buffer, std::size_t buffer_size, bool validate) {
    if (buffer == nullptr) {
        throw SuperblockSerializationError("Cannot deserialize superblock: buffer pointer is null");
    }

    if (buffer_size < DEFAULT_BLOCK_SIZE) {
        throw SuperblockSerializationError("Cannot deserialize superblock: buffer size (" +
                                           std::to_string(buffer_size) + ") is smaller than block size (" +
                                           std::to_string(DEFAULT_BLOCK_SIZE) + ")");
    }

    const auto* src = static_cast<const std::uint8_t*>(buffer);
    Superblock sb;

    sb.magic = read_u32_le(src + offsets::MAGIC);
    sb.version = read_u32_le(src + offsets::VERSION);
    sb.block_size = read_u32_le(src + offsets::BLOCK_SIZE);
    sb.inode_size = read_u32_le(src + offsets::INODE_SIZE);
    sb.total_blocks = read_u64_le(src + offsets::TOTAL_BLOCKS);
    sb.total_inodes = read_u64_le(src + offsets::TOTAL_INODES);
    sb.free_blocks = read_u64_le(src + offsets::FREE_BLOCKS);
    sb.free_inodes = read_u64_le(src + offsets::FREE_INODES);
    sb.root_inode = read_u64_le(src + offsets::ROOT_INODE);
    sb.block_bitmap_start = read_u64_le(src + offsets::BLOCK_BITMAP_START);
    sb.block_bitmap_blocks = read_u64_le(src + offsets::BLOCK_BITMAP_BLOCKS);
    sb.inode_bitmap_start = read_u64_le(src + offsets::INODE_BITMAP_START);
    sb.inode_bitmap_blocks = read_u64_le(src + offsets::INODE_BITMAP_BLOCKS);
    sb.inode_table_start = read_u64_le(src + offsets::INODE_TABLE_START);
    sb.inode_table_blocks = read_u64_le(src + offsets::INODE_TABLE_BLOCKS);
    sb.data_region_start = read_u64_le(src + offsets::DATA_REGION_START);
    sb.data_region_blocks = read_u64_le(src + offsets::DATA_REGION_BLOCKS);

    if (validate) {
        sb.validate();
    }

    return sb;
}

Superblock Superblock::read(filler::storage::BlockDevice& device, bool check_device_size) {
    alignas(8) std::uint8_t buffer[DEFAULT_BLOCK_SIZE];
    device.read_block(SUPERBLOCK_BLOCK_ID, buffer, sizeof(buffer));

    Superblock sb = Superblock::deserialize(buffer, sizeof(buffer), /*validate=*/true);

    if (check_device_size && sb.total_blocks != device.total_blocks()) {
        throw SuperblockValidationError("Superblock total_blocks (" + std::to_string(sb.total_blocks) +
                                        ") does not match device total blocks (" +
                                        std::to_string(device.total_blocks()) + ")");
    }

    return sb;
}

void Superblock::write(filler::storage::BlockDevice& device, bool check_device_size) const {
    validate();

    if (check_device_size && total_blocks != device.total_blocks()) {
        throw SuperblockValidationError("Superblock total_blocks (" + std::to_string(total_blocks) +
                                        ") does not match device total blocks (" +
                                        std::to_string(device.total_blocks()) + ")");
    }

    alignas(8) std::uint8_t buffer[DEFAULT_BLOCK_SIZE];
    serialize(buffer, sizeof(buffer));
    device.write_block(SUPERBLOCK_BLOCK_ID, buffer, sizeof(buffer));
}

bool Superblock::operator==(const Superblock& other) const noexcept {
    return magic == other.magic &&
           version == other.version &&
           block_size == other.block_size &&
           inode_size == other.inode_size &&
           total_blocks == other.total_blocks &&
           total_inodes == other.total_inodes &&
           free_blocks == other.free_blocks &&
           free_inodes == other.free_inodes &&
           root_inode == other.root_inode &&
           block_bitmap_start == other.block_bitmap_start &&
           block_bitmap_blocks == other.block_bitmap_blocks &&
           inode_bitmap_start == other.inode_bitmap_start &&
           inode_bitmap_blocks == other.inode_bitmap_blocks &&
           inode_table_start == other.inode_table_start &&
           inode_table_blocks == other.inode_table_blocks &&
           data_region_start == other.data_region_start &&
           data_region_blocks == other.data_region_blocks;
}

bool Superblock::operator!=(const Superblock& other) const noexcept {
    return !(*this == other);
}

} // namespace filler::fs
