#include "fs/BlockBitmap.hpp"
#include "fs/DirectoryManager.hpp"
#include "fs/FileManager.hpp"
#include "fs/Formatter.hpp"
#include "fs/Inode.hpp"
#include "fs/InodeManager.hpp"
#include "fs/Superblock.hpp"
#include "storage/BlockDevice.hpp"

// C++ standard library
#include <algorithm>
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

using filler::fs::DirectoryManager;
using filler::fs::FileType;
using filler::fs::FileManager;
using filler::fs::FileManagerError;
using filler::fs::FileNotFoundException;
using filler::fs::Formatter;
using filler::fs::Inode;
using filler::fs::Superblock;
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
    assert_throws_impl<ExceptionType>([&]() { (void)(expr); }, #expr, #ExceptionType, __FILE__, __LINE__)

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

std::unique_ptr<ScopedTestFile> create_formatted_disk_with_root(const std::string& filename) {
    auto file = std::make_unique<ScopedTestFile>(filename);
    file->create(Formatter::DEFAULT_DISK_SIZE, 0);
    BlockDevice dev(file->path());
    Formatter::format(dev);
    DirectoryManager dm(dev);
    dm.initialize_root();
    return file;
}

// ============================================================================
// Test Implementations
// ============================================================================

// 1. Creating an empty file
void test_creating_empty_file() {
    auto file = create_formatted_disk_with_root("build/test_fm_create.img");
    BlockDevice dev(file->path());
    FileManager fm(dev);

    const std::uint64_t file_ino = fm.create(1, "notes.txt", 0644);
    ASSERT_TRUE(file_ino >= 2);

    Inode st = fm.stat(file_ino);
    ASSERT_TRUE(st.is_regular_file());
    ASSERT_FALSE(st.is_directory());
    ASSERT_EQ(st.size_bytes, 0ULL);
    ASSERT_EQ(st.blocks_count, 0ULL);
    ASSERT_EQ(st.permissions, 0644U);

    // Verify lookup in DirectoryManager
    ASSERT_EQ(fm.directory_manager().lookup(1, "notes.txt"), file_ino);
}

// 2. Writing and reading data
void test_write_and_read_data() {
    auto file = create_formatted_disk_with_root("build/test_fm_write_read.img");
    BlockDevice dev(file->path());
    FileManager fm(dev);

    const std::uint64_t file_ino = fm.create(1, "msg.txt");
    const std::string payload = "Hello, Filler File System!";

    const std::size_t written = fm.write(file_ino, 0, payload);
    ASSERT_EQ(written, payload.size());

    Inode st = fm.stat(file_ino);
    ASSERT_EQ(st.size_bytes, payload.size());
    ASSERT_EQ(st.blocks_count, 1ULL);

    std::vector<std::uint8_t> read_bytes = fm.read(file_ino, 0, payload.size());
    std::string read_str(read_bytes.begin(), read_bytes.end());
    ASSERT_EQ(read_str, payload);
}

// 3. Partial reads and EOF behavior
void test_partial_reads() {
    auto file = create_formatted_disk_with_root("build/test_fm_partial_read.img");
    BlockDevice dev(file->path());
    FileManager fm(dev);

    const std::uint64_t file_ino = fm.create(1, "numbers.txt");
    const std::string text = "0123456789ABCDEF"; // 16 bytes
    fm.write(file_ino, 0, text);

    // Read middle chunk
    std::vector<std::uint8_t> chunk = fm.read(file_ino, 4, 6);
    ASSERT_EQ(std::string(chunk.begin(), chunk.end()), "456789");

    // Read extending past EOF (offset 10, request 20 bytes -> reads remaining 6 bytes)
    std::vector<std::uint8_t> tail = fm.read(file_ino, 10, 20);
    ASSERT_EQ(std::string(tail.begin(), tail.end()), "ABCDEF");

    // Read starting exactly at EOF
    std::vector<std::uint8_t> eof = fm.read(file_ino, 16, 10);
    ASSERT_TRUE(eof.empty());

    // Read beyond EOF
    std::vector<std::uint8_t> past_eof = fm.read(file_ino, 100, 10);
    ASSERT_TRUE(past_eof.empty());
}

// 4. Partial overwrites
void test_partial_overwrites() {
    auto file = create_formatted_disk_with_root("build/test_fm_overwrite.img");
    BlockDevice dev(file->path());
    FileManager fm(dev);

    const std::uint64_t file_ino = fm.create(1, "data.txt");
    fm.write(file_ino, 0, "AAAAAAAAAA"); // 10 A's

    // Overwrite offset 4 with "BB"
    fm.write(file_ino, 4, "BB");

    std::vector<std::uint8_t> result = fm.read(file_ino, 0, 10);
    ASSERT_EQ(std::string(result.begin(), result.end()), "AAAABBAAAA");
}

// 5. Appending data
void test_appending_data() {
    auto file = create_formatted_disk_with_root("build/test_fm_append.img");
    BlockDevice dev(file->path());
    FileManager fm(dev);

    const std::uint64_t file_ino = fm.create(1, "log.txt");
    fm.append(file_ino, "Line 1\n");
    fm.append(file_ino, "Line 2\n");
    fm.append(file_ino, "Line 3\n");

    const std::string expected = "Line 1\nLine 2\nLine 3\n";
    Inode st = fm.stat(file_ino);
    ASSERT_EQ(st.size_bytes, expected.size());

    std::vector<std::uint8_t> read_bytes = fm.read(file_ino, 0, expected.size());
    ASSERT_EQ(std::string(read_bytes.begin(), read_bytes.end()), expected);
}

// 6. Files spanning multiple data blocks
void test_files_spanning_multiple_blocks() {
    auto file = create_formatted_disk_with_root("build/test_fm_multi_block.img");
    BlockDevice dev(file->path());
    FileManager fm(dev);

    const std::uint64_t file_ino = fm.create(1, "large.bin");

    // 10,000 bytes spans across 3 blocks (4096 * 2 = 8192, 10000 - 8192 = 1808)
    constexpr std::size_t TOTAL_SIZE = 10000;
    std::vector<std::uint8_t> write_data(TOTAL_SIZE);
    for (std::size_t i = 0; i < TOTAL_SIZE; ++i) {
        write_data[i] = static_cast<std::uint8_t>((i * 7 + 13) % 256);
    }

    fm.write(file_ino, 0, write_data);

    Inode st = fm.stat(file_ino);
    ASSERT_EQ(st.size_bytes, TOTAL_SIZE);
    ASSERT_EQ(st.blocks_count, 3ULL);

    std::vector<std::uint8_t> read_data = fm.read(file_ino, 0, TOTAL_SIZE);
    ASSERT_EQ(read_data.size(), TOTAL_SIZE);
    ASSERT_TRUE(write_data == read_data);
}

// 7. Writing beyond current EOF creates zero-filled sparse gap
void test_writing_beyond_current_eof() {
    auto file = create_formatted_disk_with_root("build/test_fm_sparse_write.img");
    BlockDevice dev(file->path());
    FileManager fm(dev);

    const std::uint64_t file_ino = fm.create(1, "sparse.bin");

    // Write 4 bytes at offset 5000 (crosses block boundary, leaves bytes 0..4999 as zero)
    const std::string text = "TEST";
    fm.write(file_ino, 5000, text);

    Inode st = fm.stat(file_ino);
    ASSERT_EQ(st.size_bytes, 5004ULL);

    // Verify leading bytes are 0
    std::vector<std::uint8_t> head = fm.read(file_ino, 0, 5000);
    ASSERT_EQ(head.size(), 5000ULL);
    for (std::size_t i = 0; i < head.size(); ++i) {
        ASSERT_EQ(head[i], 0x00U);
    }

    // Verify written data
    std::vector<std::uint8_t> tail = fm.read(file_ino, 5000, 4);
    ASSERT_EQ(std::string(tail.begin(), tail.end()), text);
}

// 8. Truncating to smaller and larger sizes
void test_truncating_sizes() {
    auto file = create_formatted_disk_with_root("build/test_fm_truncate.img");
    BlockDevice dev(file->path());
    FileManager fm(dev);

    const std::uint64_t file_ino = fm.create(1, "trunc.bin");

    // Write 12,000 bytes (3 blocks)
    std::vector<std::uint8_t> data(12000, 0xAA);
    fm.write(file_ino, 0, data);
    ASSERT_EQ(fm.stat(file_ino).blocks_count, 3ULL);

    const std::uint64_t free_before_shrink = fm.block_bitmap().count_free_blocks();

    // Truncate down to 5,000 bytes (2 blocks) -> 1 block released
    fm.truncate(file_ino, 5000);
    ASSERT_EQ(fm.stat(file_ino).size_bytes, 5000ULL);
    ASSERT_EQ(fm.stat(file_ino).blocks_count, 2ULL);
    ASSERT_EQ(fm.block_bitmap().count_free_blocks(), free_before_shrink + 1ULL);

    // Truncate down to 0 bytes -> all blocks released
    fm.truncate(file_ino, 0);
    ASSERT_EQ(fm.stat(file_ino).size_bytes, 0ULL);
    ASSERT_EQ(fm.stat(file_ino).blocks_count, 0ULL);

    // Truncate up to 8,192 bytes -> 2 blocks allocated
    fm.truncate(file_ino, 8192);
    ASSERT_EQ(fm.stat(file_ino).size_bytes, 8192ULL);
    ASSERT_EQ(fm.stat(file_ino).blocks_count, 2ULL);

    // Verify newly grown file reads as zeros
    std::vector<std::uint8_t> zeros = fm.read(file_ino, 0, 8192);
    for (std::uint8_t b : zeros) {
        ASSERT_EQ(b, 0x00U);
    }
}

// 9. Removing a file and verifying resource release
void test_removing_file_and_resource_release() {
    auto file = create_formatted_disk_with_root("build/test_fm_remove.img");
    BlockDevice dev(file->path());
    FileManager fm(dev);

    const std::uint64_t initial_free_inodes = fm.inode_manager().count_free_inodes();
    const std::uint64_t initial_free_blocks = fm.block_bitmap().count_free_blocks();

    const std::uint64_t file_ino = fm.create(1, "discard.bin");
    std::vector<std::uint8_t> payload(8192, 0x55); // 2 blocks
    fm.write(file_ino, 0, payload);

    ASSERT_EQ(fm.inode_manager().count_free_inodes(), initial_free_inodes - 1ULL);
    ASSERT_EQ(fm.block_bitmap().count_free_blocks(), initial_free_blocks - 2ULL);

    fm.remove(1, "discard.bin");

    // Inodes and blocks must be fully restored
    ASSERT_EQ(fm.inode_manager().count_free_inodes(), initial_free_inodes);
    ASSERT_EQ(fm.block_bitmap().count_free_blocks(), initial_free_blocks);

    // File lookup must fail
    ASSERT_THROWS(fm.directory_manager().lookup(1, "discard.bin"), filler::fs::EntryNotFoundError);
    ASSERT_THROWS(fm.remove(1, "discard.bin"), FileNotFoundException);
    ASSERT_THROWS(fm.stat(file_ino), FileManagerError);
}

// 10. Persistence after reopening disk image
void test_persistence_after_reopening() {
    auto file = create_formatted_disk_with_root("build/test_fm_persist.img");

    std::uint64_t saved_file_ino = 0;
    const std::string content = "Persistent file content across sessions!";

    // Session 1: create and write
    {
        BlockDevice dev(file->path());
        FileManager fm(dev);
        saved_file_ino = fm.create(1, "persisted.txt");
        fm.write(saved_file_ino, 0, content);
        dev.close();
    }

    // Session 2: reopen and verify
    {
        BlockDevice dev(file->path());
        FileManager fm(dev);

        std::uint64_t looked_up = fm.directory_manager().lookup(1, "persisted.txt");
        ASSERT_EQ(looked_up, saved_file_ino);

        Inode st = fm.stat(saved_file_ino);
        ASSERT_EQ(st.size_bytes, content.size());

        std::vector<std::uint8_t> read_bytes = fm.read(saved_file_ino, 0, content.size());
        ASSERT_EQ(std::string(read_bytes.begin(), read_bytes.end()), content);
    }
}

// 11. Single indirect block support (> 64 KiB)
void test_single_indirect_blocks() {
    auto file = create_formatted_disk_with_root("build/test_fm_indirect.img");
    BlockDevice dev(file->path());
    FileManager fm(dev);

    const std::uint64_t file_ino = fm.create(1, "bigfile.dat");

    // 16 direct blocks = 64 KiB. Write 72 KiB (18 blocks).
    // Blocks 0..15 are direct, blocks 16..17 use single_indirect.
    constexpr std::size_t BIG_SIZE = 72 * 1024;
    std::vector<std::uint8_t> big_data(BIG_SIZE);
    for (std::size_t i = 0; i < BIG_SIZE; ++i) {
        big_data[i] = static_cast<std::uint8_t>(i & 0xFF);
    }

    fm.write(file_ino, 0, big_data);

    Inode st = fm.stat(file_ino);
    ASSERT_EQ(st.size_bytes, BIG_SIZE);
    // 18 data blocks + 1 indirect block pointer table = 19 blocks
    ASSERT_TRUE(st.single_indirect != 0);
    ASSERT_EQ(st.blocks_count, 19ULL);

    std::vector<std::uint8_t> read_data = fm.read(file_ino, 0, BIG_SIZE);
    ASSERT_TRUE(big_data == read_data);

    // Truncate back to 0 frees all 18 data blocks + indirect block
    const std::uint64_t free_before = fm.block_bitmap().count_free_blocks();
    fm.truncate(file_ino, 0);
    ASSERT_EQ(fm.block_bitmap().count_free_blocks(), free_before + 19ULL);
    ASSERT_EQ(fm.stat(file_ino).single_indirect, 0ULL);
}

// 12. Invalid operations and error handling
void test_invalid_operations_and_error_handling() {
    auto file = create_formatted_disk_with_root("build/test_fm_invalid.img");
    BlockDevice dev(file->path());
    FileManager fm(dev);

    // Invalid parent inode ID
    ASSERT_THROWS(fm.create(256, "file.txt"), FileManagerError);
    ASSERT_THROWS(fm.create(50, "file.txt"), FileManagerError); // unallocated parent

    // Creating inside regular file instead of directory
    const std::uint64_t f1 = fm.create(1, "parent_file.txt");
    ASSERT_THROWS(fm.create(f1, "child.txt"), FileManagerError);

    // Duplicate filename
    ASSERT_THROWS(fm.create(1, "parent_file.txt"), FileManagerError);

    // Invalid filenames
    ASSERT_THROWS(fm.create(1, ""), FileManagerError);
    ASSERT_THROWS(fm.create(1, "."), FileManagerError);
    ASSERT_THROWS(fm.create(1, ".."), FileManagerError);
    ASSERT_THROWS(fm.create(1, "path/sep"), FileManagerError);

    // Read/write on directory
    ASSERT_THROWS(fm.read(1, 0, 10), FileManagerError);
    ASSERT_THROWS(fm.write(1, 0, "test"), FileManagerError);
    ASSERT_THROWS(fm.truncate(1, 0), FileManagerError);

    // Null buffer
    ASSERT_THROWS(fm.read(f1, 0, nullptr, 10), FileManagerError);
    ASSERT_THROWS(fm.write(f1, 0, nullptr, 10), FileManagerError);

    // Removing nonexistent file
    ASSERT_THROWS(fm.remove(1, "ghost.txt"), FileNotFoundException);

    // Removing directory with file manager
    fm.directory_manager().mkdir(1, "folder");
    ASSERT_THROWS(fm.remove(1, "folder"), FileManagerError);
}

void test_truncate_growth_and_rollback() {
    auto file = create_formatted_disk_with_root("build/test_fm_truncate_rollback.img");
    BlockDevice dev(file->path());
    FileManager fm(dev);

    const std::uint64_t fid = fm.create(1, "grow_test.txt");

    // 1. Successful file growth & 2. Growth requiring multiple blocks
    // Grow from 0 to 3 blocks (12,288 bytes)
    fm.truncate(fid, 12288);
    Inode in1 = fm.stat(fid);
    ASSERT_EQ(in1.size_bytes, 12288ULL);
    ASSERT_EQ(in1.blocks_count, 3ULL);

    // Write distinctive data across all 3 blocks
    std::string test_data(12288, 'X');
    for (std::size_t i = 0; i < test_data.size(); ++i) {
        test_data[i] = static_cast<char>('A' + (i % 26));
    }
    fm.write(fid, 0, test_data);

    // Verify written content matches
    auto read_back = fm.read(fid, 0, 12288);
    ASSERT_EQ(read_back.size(), 12288ULL);
    ASSERT_EQ(std::memcmp(read_back.data(), test_data.data(), 12288), 0);

    // 3. Allocation failure after at least one successful allocation
    // We exhaust the BlockBitmap until exactly 2 free blocks remain.
    std::vector<std::uint64_t> filler_blocks;
    while (fm.block_bitmap().count_free_blocks() > 2) {
        filler_blocks.push_back(fm.block_bitmap().allocate());
    }
    ASSERT_EQ(fm.block_bitmap().count_free_blocks(), 2ULL);

    const std::uint64_t free_before = fm.block_bitmap().count_free_blocks();
    const Inode stat_before = fm.stat(fid);

    // Requesting 5 additional blocks (need 5, but only 2 free)
    // Old blocks = 3. New size = 8 * 4096 = 32768.
    // truncate will allocate 2 blocks, then fail on the 3rd!
    ASSERT_THROWS(fm.truncate(fid, 32768), filler::fs::BlockBitmapError);

    // 5. Verification that no blocks leak after failure
    const std::uint64_t free_after = fm.block_bitmap().count_free_blocks();
    ASSERT_EQ(free_after, free_before);

    // Inode metadata remained unchanged
    const Inode stat_after = fm.stat(fid);
    ASSERT_EQ(stat_after.size_bytes, stat_before.size_bytes);
    ASSERT_EQ(stat_after.blocks_count, stat_before.blocks_count);
    for (std::size_t b = 0; b < Inode::DIRECT_BLOCKS_COUNT; ++b) {
        ASSERT_EQ(stat_after.direct_blocks[b], stat_before.direct_blocks[b]);
    }
    ASSERT_EQ(stat_after.single_indirect, stat_before.single_indirect);
    ASSERT_EQ(stat_after.double_indirect, stat_before.double_indirect);

    // 6. Verification that original file contents remain unchanged after failure
    auto content_after_fail = fm.read(fid, 0, 12288);
    ASSERT_EQ(content_after_fail.size(), 12288ULL);
    ASSERT_EQ(std::memcmp(content_after_fail.data(), test_data.data(), 12288), 0);

    // Cleanup dummy filler blocks so device can be cleanly reused/closed
    for (std::uint64_t blk : filler_blocks) {
        fm.block_bitmap().free(blk);
    }

    // 7. Failure during growth through an existing single-indirect table
    {
        const std::uint64_t fid_s = fm.create(1, "single_indir_test.txt");
        // Direct blocks = 16 (0..15). Create a file with 18 blocks so single_indirect is allocated
        // and contains 2 block pointers (indices 0 and 1).
        fm.truncate(fid_s, 18 * BlockDevice::BLOCK_SIZE);
        const Inode s_before = fm.stat(fid_s);
        ASSERT_EQ(s_before.blocks_count, 19ULL); // 16 direct + 1 single_indirect table + 2 data blocks
        ASSERT_TRUE(s_before.single_indirect != 0);

        // Write test pattern across the 18 blocks
        std::string s_data(18 * BlockDevice::BLOCK_SIZE, 'S');
        for (std::size_t i = 0; i < s_data.size(); ++i) {
            s_data[i] = static_cast<char>('a' + (i % 26));
        }
        fm.write(fid_s, 0, s_data);

        // Read single indirect block from disk
        alignas(8) std::uint8_t table_before[BlockDevice::BLOCK_SIZE];
        dev.read_block(s_before.single_indirect, table_before, sizeof(table_before));
        uint64_t ptr0_before = 0, ptr1_before = 0, ptr2_before = 0;
        std::memcpy(&ptr0_before, table_before + 0, 8);
        std::memcpy(&ptr1_before, table_before + 8, 8);
        std::memcpy(&ptr2_before, table_before + 16, 8);
        ASSERT_TRUE(ptr0_before != 0);
        ASSERT_TRUE(ptr1_before != 0);
        ASSERT_EQ(ptr2_before, 0ULL);

        // Exhaust blocks to exactly 2 free blocks
        std::vector<std::uint64_t> filler;
        while (fm.block_bitmap().count_free_blocks() > 2) {
            filler.push_back(fm.block_bitmap().allocate());
        }
        ASSERT_EQ(fm.block_bitmap().count_free_blocks(), 2ULL);
        const std::uint64_t free_before_s = fm.block_bitmap().count_free_blocks();

        // Request 5 more blocks: 18 blocks -> 23 blocks (needs 5, only 2 free)
        ASSERT_THROWS(fm.truncate(fid_s, 23 * BlockDevice::BLOCK_SIZE), filler::fs::BlockBitmapError);

        // Free blocks restored
        ASSERT_EQ(fm.block_bitmap().count_free_blocks(), free_before_s);

        // Inode metadata restored
        const Inode s_after = fm.stat(fid_s);
        ASSERT_EQ(s_after.size_bytes, s_before.size_bytes);
        ASSERT_EQ(s_after.blocks_count, s_before.blocks_count);
        ASSERT_EQ(s_after.single_indirect, s_before.single_indirect);

        // Table on disk restored: entry 2 must STILL be 0, NOT pointing to a freed block!
        alignas(8) std::uint8_t table_after[BlockDevice::BLOCK_SIZE];
        dev.read_block(s_after.single_indirect, table_after, sizeof(table_after));
        uint64_t ptr0_after = 0, ptr1_after = 0, ptr2_after = 0;
        std::memcpy(&ptr0_after, table_after + 0, 8);
        std::memcpy(&ptr1_after, table_after + 8, 8);
        std::memcpy(&ptr2_after, table_after + 16, 8);
        ASSERT_EQ(ptr0_after, ptr0_before);
        ASSERT_EQ(ptr1_after, ptr1_before);
        ASSERT_EQ(ptr2_after, 0ULL);

        // Content unchanged
        auto read_s = fm.read(fid_s, 0, 18 * BlockDevice::BLOCK_SIZE);
        ASSERT_EQ(read_s.size(), s_data.size());
        ASSERT_EQ(std::memcmp(read_s.data(), s_data.data(), s_data.size()), 0);

        for (uint64_t b : filler) {
            fm.block_bitmap().free(b);
        }
    }

    // 8. Failure during growth through an existing double-indirect table
    {
        const std::uint64_t fid_d = fm.create(1, "dbl_indir_test.txt");
        // 16 direct + 512 single indirect = 528.
        // 530 blocks reaches into double indirect L2 block (entries 0 and 1).
        fm.truncate(fid_d, 530 * BlockDevice::BLOCK_SIZE);
        const Inode d_before = fm.stat(fid_d);
        ASSERT_TRUE(d_before.double_indirect != 0);

        // Read L1 block to get L2 block ID
        alignas(8) std::uint8_t l1_buf[BlockDevice::BLOCK_SIZE];
        dev.read_block(d_before.double_indirect, l1_buf, sizeof(l1_buf));
        uint64_t l2_blk = 0;
        std::memcpy(&l2_blk, l1_buf, 8);
        ASSERT_TRUE(l2_blk != 0);

        // Read L2 block
        alignas(8) std::uint8_t l2_buf_before[BlockDevice::BLOCK_SIZE];
        dev.read_block(l2_blk, l2_buf_before, sizeof(l2_buf_before));
        uint64_t l2_ptr0 = 0, l2_ptr1 = 0, l2_ptr2 = 0;
        std::memcpy(&l2_ptr0, l2_buf_before + 0, 8);
        std::memcpy(&l2_ptr1, l2_buf_before + 8, 8);
        std::memcpy(&l2_ptr2, l2_buf_before + 16, 8);
        ASSERT_TRUE(l2_ptr0 != 0);
        ASSERT_TRUE(l2_ptr1 != 0);
        ASSERT_EQ(l2_ptr2, 0ULL);

        // Exhaust to 2 free blocks
        std::vector<std::uint64_t> filler;
        while (fm.block_bitmap().count_free_blocks() > 2) {
            filler.push_back(fm.block_bitmap().allocate());
        }
        const std::uint64_t free_before_d = fm.block_bitmap().count_free_blocks();

        // Request 5 more blocks (530 -> 535)
        ASSERT_THROWS(fm.truncate(fid_d, 535 * BlockDevice::BLOCK_SIZE), filler::fs::BlockBitmapError);

        // Verify free blocks restored
        ASSERT_EQ(fm.block_bitmap().count_free_blocks(), free_before_d);

        // Inode metadata restored
        const Inode d_after = fm.stat(fid_d);
        ASSERT_EQ(d_after.size_bytes, d_before.size_bytes);
        ASSERT_EQ(d_after.blocks_count, d_before.blocks_count);
        ASSERT_EQ(d_after.double_indirect, d_before.double_indirect);

        // Verify L2 block on disk was restored and entry 2 is 0 (no freed block referenced)
        alignas(8) std::uint8_t l2_buf_after[BlockDevice::BLOCK_SIZE];
        dev.read_block(l2_blk, l2_buf_after, sizeof(l2_buf_after));
        uint64_t l2_ptr0_after = 0, l2_ptr1_after = 0, l2_ptr2_after = 0;
        std::memcpy(&l2_ptr0_after, l2_buf_after + 0, 8);
        std::memcpy(&l2_ptr1_after, l2_buf_after + 8, 8);
        std::memcpy(&l2_ptr2_after, l2_buf_after + 16, 8);
        ASSERT_EQ(l2_ptr0_after, l2_ptr0);
        ASSERT_EQ(l2_ptr1_after, l2_ptr1);
        ASSERT_EQ(l2_ptr2_after, 0ULL);

        for (uint64_t b : filler) {
            fm.block_bitmap().free(b);
        }
    }
}

void test_remove_safe_deletion_ordering() {
    auto file = create_formatted_disk_with_root("build/test_fm_remove_safe.img");
    BlockDevice dev(file->path());
    FileManager fm(dev);

    // 1. Successful file deletion & 5. Verifying inode and block resources are released correctly
    const std::uint64_t free_blocks_init = fm.block_bitmap().count_free_blocks();
    const std::uint64_t free_inodes_init = fm.inode_manager().count_free_inodes();

    const std::uint64_t fid = fm.create(1, "safe_del.txt");
    ASSERT_EQ(fm.inode_manager().count_free_inodes(), free_inodes_init - 1);

    std::string content(10000, 'Z');
    fm.write(fid, 0, content); // Allocates 3 blocks
    ASSERT_EQ(fm.block_bitmap().count_free_blocks(), free_blocks_init - 3);

    // Remove file
    fm.remove(1, "safe_del.txt");

    // Inode and block resources must be released
    ASSERT_EQ(fm.block_bitmap().count_free_blocks(), free_blocks_init);
    ASSERT_EQ(fm.inode_manager().count_free_inodes(), free_inodes_init);

    // 6. Ensuring no dangling directory entries remain
    ASSERT_THROWS(fm.directory_manager().lookup(1, "safe_del.txt"), filler::fs::EntryNotFoundError);
    const auto entries = fm.directory_manager().list(1);
    for (const auto& entry : entries) {
        ASSERT_TRUE(entry.name != "safe_del.txt");
    }

    // 2. Deleting a missing file throws FileNotFoundException
    ASSERT_THROWS(fm.remove(1, "non_existent.txt"), FileNotFoundException);

    // 3. Attempting to delete a directory using rm / FileManager::remove throws FileManagerError
    fm.directory_manager().mkdir(1, "test_dir");
    const std::uint64_t free_blocks_with_dir = fm.block_bitmap().count_free_blocks();
    const std::uint64_t free_inodes_with_dir = fm.inode_manager().count_free_inodes();

    ASSERT_THROWS(fm.remove(1, "test_dir"), FileManagerError);

    // Ensure directory entry was NOT removed and resources were NOT freed
    ASSERT_EQ(fm.block_bitmap().count_free_blocks(), free_blocks_with_dir);
    ASSERT_EQ(fm.inode_manager().count_free_inodes(), free_inodes_with_dir);
    ASSERT_EQ(fm.directory_manager().lookup(1, "test_dir") > 0, true);

    // 4. Failure during directory-entry removal (e.g. invalid parent)
    ASSERT_THROWS(fm.remove(9999, "safe_del.txt"), FileManagerError);

    // 7. Genuine failure injection during directory-entry removal
    {
        class FailingDirectoryManager : public filler::fs::DirectoryManager {
        public:
            using filler::fs::DirectoryManager::DirectoryManager;
            bool inject_remove_failure{false};

            void remove_entry(std::uint64_t parent_inode_id, const std::string& name) override {
                if (inject_remove_failure) {
                    throw filler::fs::DirectoryManagerError("Injected failure during directory entry removal");
                }
                filler::fs::DirectoryManager::remove_entry(parent_inode_id, name);
            }
        };

        filler::fs::InodeManager im(dev);
        filler::fs::BlockBitmap bm(dev);
        FailingDirectoryManager fdm(dev, im, bm);
        FileManager fm_inject(dev, im, bm, fdm);

        const std::uint64_t free_b_start = bm.count_free_blocks();
        const std::uint64_t free_i_start = im.count_free_inodes();

        const std::uint64_t fid_fail = fm_inject.create(1, "fail_del.txt");
        std::string fail_data(10000, 'F');
        fm_inject.write(fid_fail, 0, fail_data); // Allocates 3 blocks

        const std::uint64_t free_b_after_write = bm.count_free_blocks();
        const std::uint64_t free_i_after_write = im.count_free_inodes();

        // Inject removal failure
        fdm.inject_remove_failure = true;

        ASSERT_THROWS(fm_inject.remove(1, "fail_del.txt"), FileManagerError);

        // Verify:
        // 1. Directory entry was NOT unlinked / still exists in parent directory
        ASSERT_EQ(fdm.lookup(1, "fail_del.txt"), fid_fail);

        // 2. Inode was NOT prematurely deallocated
        ASSERT_TRUE(im.is_inode_allocated(fid_fail));
        ASSERT_EQ(im.count_free_inodes(), free_i_after_write);

        // 3. File data blocks were NOT prematurely deallocated
        ASSERT_EQ(bm.count_free_blocks(), free_b_after_write);

        // 4. File data is completely intact
        auto read_fail = fm_inject.read(fid_fail, 0, 10000);
        ASSERT_EQ(read_fail.size(), fail_data.size());
        ASSERT_EQ(std::memcmp(read_fail.data(), fail_data.data(), fail_data.size()), 0);

        // Turn off failure injection and verify clean removal succeeds
        fdm.inject_remove_failure = false;
        fm_inject.remove(1, "fail_del.txt");
        ASSERT_EQ(bm.count_free_blocks(), free_b_start);
        ASSERT_EQ(im.count_free_inodes(), free_i_start);

        /*
         * Note on Orphan-Resource Risk without Journaling:
         * In a filesystem without write-ahead logging or journaling (like Filler), operations
         * spanning multiple metadata blocks (directory entry, inode bitmap, block bitmap)
         * cannot achieve hardware-level atomicity across sudden power failure or kernel panics.
         *
         * By unlinking the directory entry BEFORE deallocating the inode and data blocks:
         * - If directory entry removal fails (as tested above), all file resources remain
         *   fully allocated, tracked, and accessible. No data is lost.
         * - If a crash occurs AFTER the directory entry is removed but BEFORE the inode/blocks
         *   are returned to the free bitmaps, those blocks and inode become "orphaned"
         *   (allocated in bitmaps, but not referenced in any directory path).
         * - This is the standard Unix filesystem design trade-off (e.g. classical ext2/UFS):
         *   orphaned resources cause a temporary space leak that an offline filesystem checker
         *   (fsck) can safely reclaim into lost+found or free bitmaps, whereas freeing blocks
         *   first would risk severe corruption (dangling directory pointers referencing
         *   reallocated blocks of another file).
         */
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
    std::cout << "                Filler FileSystem: FileManager Test Suite             \n";
    std::cout << "======================================================================\n";

    TestRunner runner;

    runner.run("1. Creating an empty file", test_creating_empty_file);
    runner.run("2. Writing and reading data", test_write_and_read_data);
    runner.run("3. Partial reads and EOF behavior", test_partial_reads);
    runner.run("4. Partial overwrites", test_partial_overwrites);
    runner.run("5. Appending data", test_appending_data);
    runner.run("6. Files spanning multiple data blocks", test_files_spanning_multiple_blocks);
    runner.run("7. Writing beyond current EOF (sparse gap)", test_writing_beyond_current_eof);
    runner.run("8. Truncating to smaller and larger sizes", test_truncating_sizes);
    runner.run("9. Removing a file and verifying resource release", test_removing_file_and_resource_release);
    runner.run("10. Persistence after reopening disk image", test_persistence_after_reopening);
    runner.run("11. Single indirect block support (> 64 KiB)", test_single_indirect_blocks);
    runner.run("12. Invalid operations and error handling", test_invalid_operations_and_error_handling);
    runner.run("13. Truncate growth and allocation rollback on failure", test_truncate_growth_and_rollback);
    runner.run("14. Safe deletion ordering and resource release", test_remove_safe_deletion_ordering);

    std::cout << "======================================================================\n";
    std::cout << "Test Summary: " << runner.passed << " / " << runner.total << " passed ("
              << runner.failed << " failed)\n";
    std::cout << "======================================================================\n";

    return (runner.failed == 0) ? 0 : 1;
}
