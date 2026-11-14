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

    std::cout << "======================================================================\n";
    std::cout << "Test Summary: " << runner.passed << " / " << runner.total << " passed ("
              << runner.failed << " failed)\n";
    std::cout << "======================================================================\n";

    return (runner.failed == 0) ? 0 : 1;
}
