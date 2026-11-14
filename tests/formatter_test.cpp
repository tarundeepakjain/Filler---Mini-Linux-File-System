#include "fs/Formatter.hpp"
#include "fs/Superblock.hpp"
#include "storage/BlockDevice.hpp"

// POSIX and C system headers
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// C++ standard library
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using filler::fs::Formatter;
using filler::fs::FormatterError;
using filler::fs::Superblock;
using filler::fs::SuperblockValidationError;
using filler::storage::BlockDevice;

// ============================================================================
// Lightweight Test Harness & Assertion Macros
// ============================================================================

class TestFailure : public std::runtime_error {
public:
    explicit TestFailure(const std::string& msg, const char* file, int line)
        : std::runtime_error(std::string(file) + ":" + std::to_string(line) + " - " + msg) {}
};

template <typename T>
std::string to_string_repr(const T& val) {
    if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
        return val ? "true" : "false";
    } else if constexpr (std::is_arithmetic_v<std::decay_t<T>>) {
        return std::to_string(val);
    } else if constexpr (std::is_convertible_v<T, std::string>) {
        return std::string(val);
    } else {
        std::ostringstream oss;
        oss << val;
        return oss.str();
    }
}

template <typename ExpectedException, typename Fn>
void assert_throws_impl(Fn&& fn, const char* expr_str, const char* expected_type_str, const char* file, int line) {
    bool caught = false;
    try {
        fn();
    } catch (const ExpectedException&) {
        caught = true;
    } catch (const std::exception& ex) {
        throw TestFailure(std::string("Expected ") + expected_type_str + " for " + expr_str +
                          ", but caught different exception: " + ex.what(), file, line);
    } catch (...) {
        throw TestFailure(std::string("Expected ") + expected_type_str + " for " + expr_str +
                          ", but caught non-standard exception", file, line);
    }
    if (!caught) {
        throw TestFailure(std::string("Expected ") + expected_type_str + " for " + expr_str +
                          ", but no exception was thrown", file, line);
    }
}

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            throw TestFailure(#cond " evaluated to false", __FILE__, __LINE__); \
        } \
    } while (false)

#define ASSERT_FALSE(cond) \
    do { \
        if (cond) { \
            throw TestFailure(#cond " evaluated to true, expected false", __FILE__, __LINE__); \
        } \
    } while (false)

#define ASSERT_EQ(actual, expected) \
    do { \
        auto&& act = (actual); \
        auto&& exp = (expected); \
        if (!(act == exp)) { \
            throw TestFailure("Expected " #actual " == " #expected " (" + to_string_repr(act) + " != " + to_string_repr(exp) + ")", __FILE__, __LINE__); \
        } \
    } while (false)

#define ASSERT_THROWS(expr, ExceptionType) \
    assert_throws_impl<ExceptionType>([&]() { expr; }, #expr, #ExceptionType, __FILE__, __LINE__)

// ============================================================================
// Disposable Test Image Management (RAII)
// ============================================================================

void create_binary_file(const std::string& path, std::size_t size_bytes, uint8_t fill_byte = 0) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        throw std::runtime_error("Failed to create temporary file: " + path);
    }
    if (size_bytes > 0) {
        constexpr std::size_t CHUNK_SIZE = 64 * 1024;
        std::vector<char> chunk(CHUNK_SIZE, static_cast<char>(fill_byte));
        std::size_t remaining = size_bytes;
        while (remaining > 0) {
            const std::size_t to_write = std::min(remaining, CHUNK_SIZE);
            ofs.write(chunk.data(), static_cast<std::streamsize>(to_write));
            if (!ofs) {
                throw std::runtime_error("Failed writing to temporary file: " + path);
            }
            remaining -= to_write;
        }
    }
    ofs.close();
}

class ScopedTestFile {
public:
    explicit ScopedTestFile(std::string path)
        : path_(std::move(path)) {
        std::remove(path_.c_str());
    }

    ~ScopedTestFile() {
        std::remove(path_.c_str());
    }

    ScopedTestFile(const ScopedTestFile&) = delete;
    ScopedTestFile& operator=(const ScopedTestFile&) = delete;
    ScopedTestFile(ScopedTestFile&&) = delete;
    ScopedTestFile& operator=(ScopedTestFile&&) = delete;

    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

    void create(std::size_t size_bytes, uint8_t fill_byte = 0) {
        create_binary_file(path_, size_bytes, fill_byte);
    }

private:
    std::string path_;
};

// ============================================================================
// Test Implementations
// ============================================================================

// 1. Successful filesystem formatting and metadata verification
void test_successful_formatting() {
    ScopedTestFile test_img("build/test_formatter_success.img");
    test_img.create(Formatter::DEFAULT_DISK_SIZE, 0);

    BlockDevice dev(test_img.path());
    Superblock sb = Formatter::format(dev);

    // Verify all fixed parameters
    ASSERT_EQ(sb.magic, Superblock::MAGIC);
    ASSERT_EQ(sb.version, Superblock::FORMAT_VERSION);
    ASSERT_EQ(sb.block_size, Superblock::DEFAULT_BLOCK_SIZE);
    ASSERT_EQ(sb.inode_size, Superblock::DEFAULT_INODE_SIZE);
    ASSERT_EQ(sb.total_blocks, 16384ULL);
    ASSERT_EQ(sb.total_inodes, 256ULL);
    ASSERT_EQ(sb.free_blocks, 16365ULL);
    ASSERT_EQ(sb.free_inodes, 254ULL);
    ASSERT_EQ(sb.root_inode, 1ULL);

    // Verify disk layout regions
    ASSERT_EQ(sb.block_bitmap_start, 1ULL);
    ASSERT_EQ(sb.block_bitmap_blocks, 1ULL);
    ASSERT_EQ(sb.inode_bitmap_start, 2ULL);
    ASSERT_EQ(sb.inode_bitmap_blocks, 1ULL);
    ASSERT_EQ(sb.inode_table_start, 3ULL);
    ASSERT_EQ(sb.inode_table_blocks, 16ULL);
    ASSERT_EQ(sb.data_region_start, 19ULL);
    ASSERT_EQ(sb.data_region_blocks, 16365ULL);
}

// 2. Correct Superblock persistence to disk
void test_superblock_persistence() {
    ScopedTestFile test_img("build/test_formatter_sb_persist.img");
    test_img.create(Formatter::DEFAULT_DISK_SIZE, 0xFF);

    BlockDevice dev(test_img.path());
    Superblock written_sb = Formatter::format(dev);

    // Read raw block 0 and deserialize
    alignas(8) std::uint8_t buffer[BlockDevice::BLOCK_SIZE];
    dev.read_block(Superblock::SUPERBLOCK_BLOCK_ID, buffer, sizeof(buffer));

    Superblock deserialized_sb = Superblock::deserialize(buffer, sizeof(buffer));
    ASSERT_TRUE(written_sb == deserialized_sb);

    // Read through Superblock::read
    Superblock read_sb = Superblock::read(dev);
    ASSERT_TRUE(written_sb == read_sb);
}

// 3. Correct block bitmap initialization
void test_block_bitmap_initialization() {
    ScopedTestFile test_img("build/test_formatter_block_bitmap.img");
    test_img.create(Formatter::DEFAULT_DISK_SIZE, 0xAA);

    BlockDevice dev(test_img.path());
    Superblock sb = Formatter::format(dev);

    // Read block bitmap (block 1)
    std::vector<std::uint8_t> block_bitmap(BlockDevice::BLOCK_SIZE);
    dev.read_block(sb.block_bitmap_start, block_bitmap.data(), block_bitmap.size());

    // 19 metadata blocks (0 through 18) must be allocated (bit = 1)
    // Blocks 0..7 -> byte 0: 0xFF
    ASSERT_EQ(block_bitmap[0], 0xFFU);
    // Blocks 8..15 -> byte 1: 0xFF
    ASSERT_EQ(block_bitmap[1], 0xFFU);
    // Blocks 16..18 -> byte 2: bits 0, 1, 2 set -> 0b00000111 = 0x07
    ASSERT_EQ(block_bitmap[2], 0x07U);

    // Check individual bits for first 32 blocks
    for (std::size_t blk = 0; blk < 32; ++blk) {
        const std::size_t byte_idx = blk / 8;
        const std::size_t bit_idx = blk % 8;
        const bool is_set = ((block_bitmap[byte_idx] >> bit_idx) & 1U) != 0;
        if (blk < sb.data_region_start) {
            ASSERT_TRUE(is_set);
        } else {
            ASSERT_FALSE(is_set);
        }
    }

    // Check all subsequent bytes up to total_blocks (16384 blocks = 2048 bytes)
    for (std::size_t i = 3; i < 16384 / 8; ++i) {
        ASSERT_EQ(block_bitmap[i], 0x00U);
    }

    // Verify total allocated block count in the bitmap
    std::uint64_t allocated_blocks = 0;
    for (std::size_t blk = 0; blk < sb.total_blocks; ++blk) {
        const std::size_t byte_idx = blk / 8;
        const std::size_t bit_idx = blk % 8;
        if (((block_bitmap[byte_idx] >> bit_idx) & 1U) != 0) {
            ++allocated_blocks;
        }
    }
    ASSERT_EQ(allocated_blocks, 19ULL);
    ASSERT_EQ(sb.total_blocks - allocated_blocks, sb.free_blocks);
}

// 4. Inode bitmap initialization: Reserved inode 0 and Root inode 1
void test_inode_bitmap_and_allocations() {
    ScopedTestFile test_img("build/test_formatter_inode_bitmap.img");
    test_img.create(Formatter::DEFAULT_DISK_SIZE, 0x55);

    BlockDevice dev(test_img.path());
    Superblock sb = Formatter::format(dev);

    // Read inode bitmap (block 2)
    std::vector<std::uint8_t> inode_bitmap(BlockDevice::BLOCK_SIZE);
    dev.read_block(sb.inode_bitmap_start, inode_bitmap.data(), inode_bitmap.size());

    // Inode 0 (reserved) is bit 0; Inode 1 (root) is bit 1
    // Byte 0 must be 0b00000011 = 0x03
    ASSERT_EQ(inode_bitmap[0], 0x03U);

    // Verify individual bit states for all 256 inodes
    for (std::size_t ino = 0; ino < sb.total_inodes; ++ino) {
        const std::size_t byte_idx = ino / 8;
        const std::size_t bit_idx = ino % 8;
        const bool is_set = ((inode_bitmap[byte_idx] >> bit_idx) & 1U) != 0;
        if (ino == Superblock::RESERVED_INODE || ino == sb.root_inode) {
            ASSERT_TRUE(is_set);
        } else {
            ASSERT_FALSE(is_set);
        }
    }

    // All bytes beyond byte 0 up to 256 inodes (byte 31) must be 0x00
    for (std::size_t i = 1; i < 256 / 8; ++i) {
        ASSERT_EQ(inode_bitmap[i], 0x00U);
    }

    // Rest of the 4096-byte bitmap block must also be 0x00
    for (std::size_t i = 32; i < inode_bitmap.size(); ++i) {
        ASSERT_EQ(inode_bitmap[i], 0x00U);
    }

    // Total allocated inodes must be 2
    std::uint64_t allocated_inodes = 0;
    for (std::size_t ino = 0; ino < sb.total_inodes; ++ino) {
        const std::size_t byte_idx = ino / 8;
        const std::size_t bit_idx = ino % 8;
        if (((inode_bitmap[byte_idx] >> bit_idx) & 1U) != 0) {
            ++allocated_inodes;
        }
    }
    ASSERT_EQ(allocated_inodes, 2ULL);
    ASSERT_EQ(sb.total_inodes - allocated_inodes, sb.free_inodes);
}

// 5. Zero-initialized inode table verification
void test_zero_initialized_inode_table() {
    ScopedTestFile test_img("build/test_formatter_inode_table.img");
    test_img.create(Formatter::DEFAULT_DISK_SIZE, 0xEE); // Fill with non-zero dirty pattern

    BlockDevice dev(test_img.path());
    Superblock sb = Formatter::format(dev);

    // Inode table occupies blocks 3 through 18 (16 blocks)
    ASSERT_EQ(sb.inode_table_start, 3ULL);
    ASSERT_EQ(sb.inode_table_blocks, 16ULL);

    std::vector<std::uint8_t> block_buffer(BlockDevice::BLOCK_SIZE);
    for (std::uint64_t b = 0; b < sb.inode_table_blocks; ++b) {
        dev.read_block(sb.inode_table_start + b, block_buffer.data(), block_buffer.size());
        for (std::size_t byte_idx = 0; byte_idx < block_buffer.size(); ++byte_idx) {
            if (block_buffer[byte_idx] != 0x00U) {
                throw TestFailure("Inode table block " + std::to_string(sb.inode_table_start + b) +
                                  " byte " + std::to_string(byte_idx) + " is not zero (" +
                                  std::to_string(block_buffer[byte_idx]) + ")",
                                  __FILE__, __LINE__);
            }
        }
    }
}

// 6. Reopening the disk image and verifying persisted metadata across sessions
void test_reopening_and_verifying_metadata() {
    ScopedTestFile test_img("build/test_formatter_reopen.img");
    test_img.create(Formatter::DEFAULT_DISK_SIZE, 0xCC);

    Superblock original_sb;
    {
        BlockDevice dev(test_img.path());
        original_sb = Formatter::format(dev);
        dev.close();
    }

    // Reopen in a new BlockDevice instance
    {
        BlockDevice dev(test_img.path());
        ASSERT_TRUE(dev.is_open());

        Superblock read_sb = Superblock::read(dev);
        ASSERT_TRUE(original_sb == read_sb);

        // Verify block bitmap persisted
        std::vector<std::uint8_t> block_bitmap(BlockDevice::BLOCK_SIZE);
        dev.read_block(read_sb.block_bitmap_start, block_bitmap.data(), block_bitmap.size());
        ASSERT_EQ(block_bitmap[0], 0xFFU);
        ASSERT_EQ(block_bitmap[1], 0xFFU);
        ASSERT_EQ(block_bitmap[2], 0x07U);

        // Verify inode bitmap persisted
        std::vector<std::uint8_t> inode_bitmap(BlockDevice::BLOCK_SIZE);
        dev.read_block(read_sb.inode_bitmap_start, inode_bitmap.data(), inode_bitmap.size());
        ASSERT_EQ(inode_bitmap[0], 0x03U);

        // Verify inode table persisted as zeros
        std::vector<std::uint8_t> it_block(BlockDevice::BLOCK_SIZE);
        for (std::uint64_t b = 0; b < read_sb.inode_table_blocks; ++b) {
            dev.read_block(read_sb.inode_table_start + b, it_block.data(), it_block.size());
            for (std::size_t i = 0; i < it_block.size(); ++i) {
                ASSERT_EQ(it_block[i], 0x00U);
            }
        }
    }
}

// 7. Invalid disk image handling
void test_invalid_disk_image_handling() {
    // 7a. Closed / unopened BlockDevice
    {
        ScopedTestFile test_img("build/test_formatter_closed.img");
        test_img.create(Formatter::DEFAULT_DISK_SIZE, 0);

        BlockDevice dev(test_img.path());
        dev.close();
        ASSERT_FALSE(dev.is_open());

        ASSERT_THROWS(Formatter::format(dev), FormatterError);
    }

    // 7b. Disk image size mismatch with standard size enforcement (e.g. 32 MiB image)
    {
        ScopedTestFile test_img("build/test_formatter_mismatch.img");
        test_img.create(32 * 1024 * 1024, 0); // 8192 blocks instead of 16384

        BlockDevice dev(test_img.path());
        ASSERT_THROWS(Formatter::format(dev), FormatterError);
    }

    // 7c. Undersized disk image insufficient for metadata regions (< 19 blocks)
    {
        ScopedTestFile test_img("build/test_formatter_undersized.img");
        test_img.create(8 * 4096, 0); // 8 blocks

        BlockDevice dev(test_img.path());
        // Even when bypass standard size enforcement, creating default superblock must fail
        ASSERT_THROWS(Formatter::format(dev, 256, /*enforce_standard_size=*/false), SuperblockValidationError);
    }

    // 7d. Invalid inode count (< 2 inodes)
    {
        ScopedTestFile test_img("build/test_formatter_invalid_inodes.img");
        test_img.create(Formatter::DEFAULT_DISK_SIZE, 0);

        BlockDevice dev(test_img.path());
        ASSERT_THROWS(Formatter::format(dev, 1), SuperblockValidationError);
    }
}

// 8. Formatter class instance interface
void test_formatter_instance_interface() {
    ScopedTestFile test_img("build/test_formatter_instance.img");
    test_img.create(Formatter::DEFAULT_DISK_SIZE, 0);

    BlockDevice dev(test_img.path());
    Formatter formatter(dev);
    Superblock sb = formatter.format();

    ASSERT_EQ(sb.magic, Superblock::MAGIC);
    ASSERT_EQ(sb.total_blocks, 16384ULL);
    ASSERT_EQ(sb.free_blocks, 16365ULL);
}

// 9. Formatting dirty image leaves data blocks untouched
void test_formatting_dirty_image_leaves_data_blocks_untouched() {
    ScopedTestFile test_img("build/test_formatter_data_untouched.img");
    test_img.create(Formatter::DEFAULT_DISK_SIZE, 0x7E);

    BlockDevice dev(test_img.path());
    Superblock sb = Formatter::format(dev);

    // Verify first data block (block 19) is still untouched (0x7E)
    std::vector<std::uint8_t> data_block(BlockDevice::BLOCK_SIZE);
    dev.read_block(sb.data_region_start, data_block.data(), data_block.size());
    for (std::size_t i = 0; i < data_block.size(); ++i) {
        ASSERT_EQ(data_block[i], 0x7EU);
    }
}

// ============================================================================
// Test Suite Runner
// ============================================================================

class TestRunner {
public:
    int passed{0};
    int failed{0};
    int total{0};

    void run(const std::string& name, const std::function<void()>& test_fn) {
        ++total;
        try {
            test_fn();
            ++passed;
            std::cout << "  [PASS] " << name << "\n";
        } catch (const TestFailure& failure) {
            ++failed;
            std::cerr << "  [FAIL] " << name << "\n"
                      << "         Assertion failed: " << failure.what() << "\n";
        } catch (const std::exception& ex) {
            ++failed;
            std::cerr << "  [FAIL] " << name << "\n"
                      << "         Unexpected std::exception: " << ex.what() << "\n";
        } catch (...) {
            ++failed;
            std::cerr << "  [FAIL] " << name << "\n"
                      << "         Unexpected non-standard exception\n";
        }
    }
};

} // namespace

int main() {
    std::cout << "======================================================================\n";
    std::cout << "               Filler FileSystem: Formatter Test Suite                \n";
    std::cout << "======================================================================\n";

    TestRunner runner;

    runner.run("1. Successful filesystem formatting and metadata verification", test_successful_formatting);
    runner.run("2. Correct Superblock persistence to disk", test_superblock_persistence);
    runner.run("3. Correct block bitmap initialization", test_block_bitmap_initialization);
    runner.run("4. Inode bitmap initialization (reserved inode 0 and root inode 1)", test_inode_bitmap_and_allocations);
    runner.run("5. Zero-initialized inode table verification", test_zero_initialized_inode_table);
    runner.run("6. Reopening disk image and verifying persisted metadata", test_reopening_and_verifying_metadata);
    runner.run("7. Invalid disk image handling", test_invalid_disk_image_handling);
    runner.run("8. Formatter class instance interface", test_formatter_instance_interface);
    runner.run("9. Formatting dirty image leaves data blocks untouched", test_formatting_dirty_image_leaves_data_blocks_untouched);

    std::cout << "======================================================================\n";
    std::cout << "Test Summary: " << runner.passed << " / " << runner.total << " passed ("
              << runner.failed << " failed)\n";
    std::cout << "======================================================================\n";

    return (runner.failed == 0) ? 0 : 1;
}

