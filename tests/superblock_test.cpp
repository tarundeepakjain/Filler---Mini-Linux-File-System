#include "fs/Superblock.hpp"
#include "storage/BlockDevice.hpp"

// POSIX and system headers
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// C++ standard library
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using filler::fs::Superblock;
using filler::fs::SuperblockValidationError;
using filler::fs::SuperblockSerializationError;
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
        constexpr std::size_t CHUNK_SIZE = 4096;
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
// Tests
// ============================================================================

// 1. Serializing and deserializing a valid superblock
void test_serialize_deserialize_valid() {
    Superblock sb = Superblock::create_default(16384, 256);
    std::vector<std::uint8_t> buffer(BlockDevice::BLOCK_SIZE);

    sb.serialize(buffer.data(), buffer.size());
    Superblock deserialized = Superblock::deserialize(buffer.data(), buffer.size());

    ASSERT_TRUE(sb == deserialized);
}

// 2. Preserving every field through a round trip
void test_preserve_every_field_roundtrip() {
    Superblock sb;
    sb.magic = Superblock::MAGIC;
    sb.version = Superblock::FORMAT_VERSION;
    sb.block_size = Superblock::DEFAULT_BLOCK_SIZE;
    sb.inode_size = Superblock::DEFAULT_INODE_SIZE;
    sb.total_blocks = 20000;
    sb.total_inodes = 512;
    sb.free_blocks = 12345;
    sb.free_inodes = 400;
    sb.root_inode = Superblock::DEFAULT_ROOT_INODE;
    sb.block_bitmap_start = 1;
    sb.block_bitmap_blocks = 1;
    sb.inode_bitmap_start = 2;
    sb.inode_bitmap_blocks = 1;
    sb.inode_table_start = 3;
    sb.inode_table_blocks = 32;
    sb.data_region_start = 35;
    sb.data_region_blocks = 19965;

    sb.validate();

    std::vector<std::uint8_t> buffer(BlockDevice::BLOCK_SIZE, 0xFF);
    sb.serialize(buffer.data(), buffer.size());

    // Verify padding bytes (from offset 120 to 4095) were zeroed out
    for (std::size_t i = filler::fs::offsets::SERIALIZED_METADATA_SIZE; i < buffer.size(); ++i) {
        ASSERT_EQ(buffer[i], 0);
    }

    Superblock deserialized = Superblock::deserialize(buffer.data(), buffer.size());

    ASSERT_EQ(deserialized.magic, sb.magic);
    ASSERT_EQ(deserialized.version, sb.version);
    ASSERT_EQ(deserialized.block_size, sb.block_size);
    ASSERT_EQ(deserialized.inode_size, sb.inode_size);
    ASSERT_EQ(deserialized.total_blocks, sb.total_blocks);
    ASSERT_EQ(deserialized.total_inodes, sb.total_inodes);
    ASSERT_EQ(deserialized.free_blocks, sb.free_blocks);
    ASSERT_EQ(deserialized.free_inodes, sb.free_inodes);
    ASSERT_EQ(deserialized.root_inode, sb.root_inode);
    ASSERT_EQ(deserialized.block_bitmap_start, sb.block_bitmap_start);
    ASSERT_EQ(deserialized.block_bitmap_blocks, sb.block_bitmap_blocks);
    ASSERT_EQ(deserialized.inode_bitmap_start, sb.inode_bitmap_start);
    ASSERT_EQ(deserialized.inode_bitmap_blocks, sb.inode_bitmap_blocks);
    ASSERT_EQ(deserialized.inode_table_start, sb.inode_table_start);
    ASSERT_EQ(deserialized.inode_table_blocks, sb.inode_table_blocks);
    ASSERT_EQ(deserialized.data_region_start, sb.data_region_start);
    ASSERT_EQ(deserialized.data_region_blocks, sb.data_region_blocks);

    ASSERT_TRUE(sb == deserialized);
    ASSERT_FALSE(sb != deserialized);
}

// 3. Rejecting an invalid magic number
void test_reject_invalid_magic() {
    Superblock sb = Superblock::create_default(16384, 256);
    sb.magic = 0xDEADBEEFU;
    ASSERT_THROWS(sb.validate(), SuperblockValidationError);

    std::vector<std::uint8_t> buffer(BlockDevice::BLOCK_SIZE);
    sb.serialize(buffer.data(), buffer.size());
    ASSERT_THROWS(Superblock::deserialize(buffer.data(), buffer.size()), SuperblockValidationError);
}

// 4. Rejecting an unsupported version
void test_reject_unsupported_version() {
    Superblock sb = Superblock::create_default(16384, 256);

    sb.version = 0;
    ASSERT_THROWS(sb.validate(), SuperblockValidationError);

    sb.version = 2;
    ASSERT_THROWS(sb.validate(), SuperblockValidationError);
}

// 5. Rejecting an invalid block size
void test_reject_invalid_block_size() {
    Superblock sb = Superblock::create_default(16384, 256);

    sb.block_size = 1024;
    ASSERT_THROWS(sb.validate(), SuperblockValidationError);

    sb.block_size = 8192;
    ASSERT_THROWS(sb.validate(), SuperblockValidationError);

    sb.block_size = 0;
    ASSERT_THROWS(sb.validate(), SuperblockValidationError);
}

// 6. Rejecting invalid region boundaries and overlapping regions
void test_reject_invalid_region_boundaries() {
    // Block bitmap overlapping superblock at block 0
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.block_bitmap_start = 0;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }

    // Block bitmap overlapping inode bitmap
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.block_bitmap_blocks = 2; // [1, 3), but inode bitmap starts at 2
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }

    // Inode bitmap overlapping inode table
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.inode_bitmap_blocks = 2; // [2, 4), but inode table starts at 3
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }

    // Inode table overlapping data region
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.inode_table_blocks = 17; // [3, 20), but data region starts at 19
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }

    // Data region extending beyond total blocks
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.data_region_blocks = 16366; // 19 + 16366 = 16385 > 16384
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }

    // Zero-length regions
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.block_bitmap_blocks = 0;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.inode_bitmap_blocks = 0;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.inode_table_blocks = 0;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.data_region_blocks = 0;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }
}

// 7. Rejecting insufficient bitmap or inode-table capacity
void test_reject_insufficient_capacity() {
    // Block bitmap capacity: 1 block tracks 4096 * 8 = 32768 blocks.
    // 32769 total blocks with 1 block bitmap block must fail.
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.total_blocks = 32769;
        sb.block_bitmap_blocks = 1;
        sb.data_region_blocks = sb.total_blocks - sb.data_region_start;
        sb.free_blocks = sb.data_region_blocks;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }

    // Inode bitmap capacity: 1 block tracks 32768 inodes.
    // 32769 total inodes with 1 inode bitmap block must fail.
    {
        Superblock sb = Superblock::create_default(65536, 256);
        sb.total_inodes = 32769;
        sb.inode_bitmap_blocks = 1;
        sb.inode_table_blocks = (sb.total_inodes + 15) / 16;
        sb.data_region_start = sb.inode_table_start + sb.inode_table_blocks;
        sb.data_region_blocks = sb.total_blocks - sb.data_region_start;
        sb.free_blocks = sb.data_region_blocks;
        sb.free_inodes = sb.total_inodes - 2;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }

    // Inode table capacity: 15 blocks can only store 15 * 16 = 240 inodes.
    // 256 total inodes must fail with 15 inode table blocks.
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.inode_table_blocks = 15;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }
}

// 8. Rejecting invalid inode counts and root inode values
void test_reject_invalid_inode_counts_and_root() {
    // Total inodes is zero
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.total_inodes = 0;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }

    // Root inode is reserved inode 0
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.root_inode = 0;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }

    // Root inode is non-standard
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.root_inode = 2;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }

    // Root inode out of range
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.total_inodes = 1;
        sb.root_inode = 1;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }
}

// 9. Reading and writing a superblock through BlockDevice
void test_read_write_superblock_via_block_device() {
    ScopedTestFile test_img("build/test_sb_device.img");
    test_img.create(64 * 1024 * 1024, 0); // 64 MiB = 16384 blocks

    Superblock written_sb = Superblock::create_default(16384, 256);

    // Write superblock
    {
        BlockDevice dev(test_img.path());
        written_sb.write(dev);
    }

    // Read back in new BlockDevice session
    {
        BlockDevice dev(test_img.path());
        Superblock read_sb = Superblock::read(dev);
        ASSERT_TRUE(written_sb == read_sb);
    }
}

// 10. Ensuring invalid metadata is rejected without modifying unrelated image blocks
void test_invalid_metadata_rejected_without_modifying_blocks() {
    ScopedTestFile test_img("build/test_sb_unmodified.img");
    test_img.create(64 * 1024 * 1024, 0xAA); // Pre-fill image with 0xAA pattern

    // Capture initial raw blocks 0 and 1
    std::vector<std::uint8_t> initial_block0(BlockDevice::BLOCK_SIZE);
    std::vector<std::uint8_t> initial_block1(BlockDevice::BLOCK_SIZE);
    {
        BlockDevice dev(test_img.path());
        dev.read_block(0, initial_block0.data(), initial_block0.size());
        dev.read_block(1, initial_block1.data(), initial_block1.size());
    }

    // Attempt to write an invalid superblock (e.g. invalid magic number)
    {
        BlockDevice dev(test_img.path());
        Superblock invalid_sb = Superblock::create_default(16384, 256);
        invalid_sb.magic = 0xBADF00DU;

        ASSERT_THROWS(invalid_sb.write(dev), SuperblockValidationError);
    }

    // Verify neither block 0 nor block 1 was modified
    {
        BlockDevice dev(test_img.path());
        std::vector<std::uint8_t> current_block0(BlockDevice::BLOCK_SIZE);
        std::vector<std::uint8_t> current_block1(BlockDevice::BLOCK_SIZE);

        dev.read_block(0, current_block0.data(), current_block0.size());
        dev.read_block(1, current_block1.data(), current_block1.size());

        ASSERT_TRUE(initial_block0 == current_block0);
        ASSERT_TRUE(initial_block1 == current_block1);
    }

    // Attempt to read corrupted superblock (blocks are filled with 0xAA)
    {
        BlockDevice dev(test_img.path());
        ASSERT_THROWS(Superblock::read(dev), SuperblockValidationError);

        // Verify block 1 remains unaffected
        std::vector<std::uint8_t> current_block1(BlockDevice::BLOCK_SIZE);
        dev.read_block(1, current_block1.data(), current_block1.size());
        ASSERT_TRUE(initial_block1 == current_block1);
    }
}

// 11. Integer overflow arithmetic protection
void test_arithmetic_overflow_protection() {
    Superblock sb = Superblock::create_default(16384, 256);
    sb.data_region_start = std::numeric_limits<std::uint64_t>::max() - 2;
    sb.data_region_blocks = 10;
    ASSERT_THROWS(sb.validate(), SuperblockValidationError);
}

// 12. Free count validation invariants
void test_free_counts_validation() {
    // free_blocks > data_region_blocks
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.free_blocks = sb.data_region_blocks + 1;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }

    // free_inodes >= total_inodes (cannot have all inodes free because root inode is allocated)
    {
        Superblock sb = Superblock::create_default(16384, 256);
        sb.free_inodes = sb.total_inodes;
        ASSERT_THROWS(sb.validate(), SuperblockValidationError);
    }
}

// 13. Buffer validation during serialization/deserialization
void test_buffer_argument_validation() {
    Superblock sb = Superblock::create_default(16384, 256);
    std::vector<std::uint8_t> small_buffer(4095);

    ASSERT_THROWS(sb.serialize(nullptr, 4096), SuperblockSerializationError);
    ASSERT_THROWS(sb.serialize(small_buffer.data(), small_buffer.size()), SuperblockSerializationError);

    ASSERT_THROWS(Superblock::deserialize(nullptr, 4096), SuperblockSerializationError);
    ASSERT_THROWS(Superblock::deserialize(small_buffer.data(), small_buffer.size()), SuperblockSerializationError);
}

// 14. Device size mismatch check
void test_device_size_mismatch() {
    ScopedTestFile test_img("build/test_sb_mismatch.img");
    test_img.create(64 * 1024 * 1024, 0); // 16384 blocks

    BlockDevice dev(test_img.path());

    // Superblock with 1000 total blocks written to 16384-block device
    Superblock mismatched_sb = Superblock::create_default(1000, 256);
    ASSERT_THROWS(mismatched_sb.write(dev, /*check_device_size=*/true), SuperblockValidationError);
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
    std::cout << "              Filler FileSystem: Superblock Test Suite                \n";
    std::cout << "======================================================================\n";

    TestRunner runner;

    runner.run("1. Serializing and deserializing a valid superblock", test_serialize_deserialize_valid);
    runner.run("2. Preserving every field through a round trip", test_preserve_every_field_roundtrip);
    runner.run("3. Rejecting an invalid magic number", test_reject_invalid_magic);
    runner.run("4. Rejecting an unsupported version", test_reject_unsupported_version);
    runner.run("5. Rejecting an invalid block size", test_reject_invalid_block_size);
    runner.run("6. Rejecting invalid region boundaries and overlapping regions", test_reject_invalid_region_boundaries);
    runner.run("7. Rejecting insufficient bitmap or inode-table capacity", test_reject_insufficient_capacity);
    runner.run("8. Rejecting invalid inode counts and root inode values", test_reject_invalid_inode_counts_and_root);
    runner.run("9. Reading and writing a superblock through BlockDevice", test_read_write_superblock_via_block_device);
    runner.run("10. Ensuring invalid metadata is rejected without modifying unrelated image blocks", test_invalid_metadata_rejected_without_modifying_blocks);
    runner.run("11. Rejecting arithmetic overflow in region boundaries", test_arithmetic_overflow_protection);
    runner.run("12. Rejecting invalid free block and inode counts", test_free_counts_validation);
    runner.run("13. Rejecting null or undersized buffers during serialization/deserialization", test_buffer_argument_validation);
    runner.run("14. Rejecting device size mismatch on read/write", test_device_size_mismatch);

    std::cout << "======================================================================\n";
    std::cout << "Test Summary: " << runner.passed << " / " << runner.total
              << " passed (" << runner.failed << " failed)\n";
    std::cout << "======================================================================\n";

    return (runner.failed == 0) ? 0 : 1;
}
