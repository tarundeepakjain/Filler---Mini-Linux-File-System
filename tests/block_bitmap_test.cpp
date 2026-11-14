#include "fs/BlockBitmap.hpp"
#include "fs/Formatter.hpp"
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

using filler::fs::BlockBitmap;
using filler::fs::BlockBitmapError;
using filler::fs::Formatter;
using filler::fs::NoFreeBlocksError;
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

// Helper to create and format a standard test disk
std::unique_ptr<ScopedTestFile> create_formatted_disk(const std::string& filename) {
    auto file = std::make_unique<ScopedTestFile>(filename);
    file->create(Formatter::DEFAULT_DISK_SIZE, 0);
    BlockDevice dev(file->path());
    Formatter::format(dev);
    return file;
}

// ============================================================================
// Test Implementations
// ============================================================================

// 1. Initial bitmap state after formatting
void test_initial_bitmap_state() {
    auto file = create_formatted_disk("build/test_bm_initial.img");
    BlockDevice dev(file->path());
    BlockBitmap bitmap(dev);

    // Initial free blocks must equal total data region blocks (16,365)
    ASSERT_EQ(bitmap.count_free_blocks(), 16365ULL);
    ASSERT_EQ(bitmap.superblock().free_blocks, 16365ULL);

    // Metadata blocks 0 to 18 must all be marked allocated
    for (std::uint64_t blk = 0; blk < 19; ++blk) {
        ASSERT_TRUE(bitmap.is_allocated(blk));
    }

    // Data blocks 19 to 16,383 must all initially be free
    for (std::uint64_t blk = 19; blk < 100; ++blk) { // Sample first 80 data blocks
        ASSERT_FALSE(bitmap.is_allocated(blk));
    }
    ASSERT_FALSE(bitmap.is_allocated(16383));
}

// 2. First allocation returns the first available data block
void test_first_allocation() {
    auto file = create_formatted_disk("build/test_bm_first_alloc.img");
    BlockDevice dev(file->path());
    BlockBitmap bitmap(dev);

    const std::uint64_t allocated_blk = bitmap.allocate();
    // First data block starts at 19
    ASSERT_EQ(allocated_blk, 19ULL);
    ASSERT_TRUE(bitmap.is_allocated(19));
    ASSERT_EQ(bitmap.count_free_blocks(), 16364ULL);
}

// 3. Multiple consecutive allocations
void test_multiple_consecutive_allocations() {
    auto file = create_formatted_disk("build/test_bm_multi_alloc.img");
    BlockDevice dev(file->path());
    BlockBitmap bitmap(dev);

    for (std::uint64_t i = 0; i < 20; ++i) {
        const std::uint64_t blk = bitmap.allocate();
        ASSERT_EQ(blk, 19ULL + i);
        ASSERT_TRUE(bitmap.is_allocated(blk));
    }

    ASSERT_EQ(bitmap.count_free_blocks(), 16365ULL - 20ULL);
}

// 4. Correct allocation status
void test_correct_allocation_status() {
    auto file = create_formatted_disk("build/test_bm_status.img");
    BlockDevice dev(file->path());
    BlockBitmap bitmap(dev);

    // Metadata blocks are allocated
    ASSERT_TRUE(bitmap.is_allocated(0));
    ASSERT_TRUE(bitmap.is_allocated(1));
    ASSERT_TRUE(bitmap.is_allocated(18));

    // Data blocks are initially free
    ASSERT_FALSE(bitmap.is_allocated(19));
    ASSERT_FALSE(bitmap.is_allocated(20));

    std::uint64_t b1 = bitmap.allocate();
    std::uint64_t b2 = bitmap.allocate();

    ASSERT_TRUE(bitmap.is_allocated(b1));
    ASSERT_TRUE(bitmap.is_allocated(b2));
    ASSERT_FALSE(bitmap.is_allocated(b2 + 1));
}

// 5. Successful deallocation
void test_successful_deallocation() {
    auto file = create_formatted_disk("build/test_bm_dealloc.img");
    BlockDevice dev(file->path());
    BlockBitmap bitmap(dev);

    std::uint64_t b1 = bitmap.allocate(); // 19
    std::uint64_t b2 = bitmap.allocate(); // 20
    std::uint64_t b3 = bitmap.allocate(); // 21

    ASSERT_EQ(bitmap.count_free_blocks(), 16362ULL);

    bitmap.free(b2); // Free block 20
    ASSERT_TRUE(bitmap.is_allocated(b1));
    ASSERT_FALSE(bitmap.is_allocated(b2));
    ASSERT_TRUE(bitmap.is_allocated(b3));
    ASSERT_EQ(bitmap.count_free_blocks(), 16363ULL);
}

// 6. Reallocation of a previously freed block
void test_reallocation_of_freed_block() {
    auto file = create_formatted_disk("build/test_bm_realloc.img");
    BlockDevice dev(file->path());
    BlockBitmap bitmap(dev);

    std::uint64_t b1 = bitmap.allocate(); // 19
    std::uint64_t b2 = bitmap.allocate(); // 20
    std::uint64_t b3 = bitmap.allocate(); // 21

    ASSERT_EQ(b1, 19ULL);
    ASSERT_EQ(b2, 20ULL);
    ASSERT_EQ(b3, 21ULL);

    bitmap.free(b2); // Free block 20

    // Next allocate should find block 20 (lowest available)
    std::uint64_t reallocated = bitmap.allocate();
    ASSERT_EQ(reallocated, 20ULL);
    ASSERT_TRUE(bitmap.is_allocated(20));

    // Subsequent allocate should proceed after b3
    std::uint64_t b4 = bitmap.allocate();
    ASSERT_EQ(b4, 22ULL);
}

// 7. Double-free rejection
void test_double_free_rejection() {
    auto file = create_formatted_disk("build/test_bm_double_free.img");
    BlockDevice dev(file->path());
    BlockBitmap bitmap(dev);

    std::uint64_t b = bitmap.allocate(); // 19
    bitmap.free(b);

    // Freeing again must throw BlockBitmapError
    ASSERT_THROWS(bitmap.free(b), BlockBitmapError);

    // Freeing an unallocated block must also throw BlockBitmapError
    ASSERT_THROWS(bitmap.free(50), BlockBitmapError);
}

// 8. Invalid block ID handling
void test_invalid_block_id_handling() {
    auto file = create_formatted_disk("build/test_bm_invalid_id.img");
    BlockDevice dev(file->path());
    BlockBitmap bitmap(dev);

    // Out of bounds free
    ASSERT_THROWS(bitmap.free(16384), BlockBitmapError);
    ASSERT_THROWS(bitmap.free(99999), BlockBitmapError);

    // Out of bounds is_allocated
    ASSERT_THROWS(bitmap.is_allocated(16384), BlockBitmapError);
    ASSERT_THROWS(bitmap.is_allocated(99999), BlockBitmapError);
}

// 9. Protection of metadata blocks
void test_protection_of_metadata_blocks() {
    auto file = create_formatted_disk("build/test_bm_protect_meta.img");
    BlockDevice dev(file->path());
    BlockBitmap bitmap(dev);

    // Attempting to free any metadata block [0, 18] must be rejected
    for (std::uint64_t blk = 0; blk < 19; ++blk) {
        ASSERT_TRUE(bitmap.is_allocated(blk));
        ASSERT_THROWS(bitmap.free(blk), BlockBitmapError);
        // Ensure status didn't change
        ASSERT_TRUE(bitmap.is_allocated(blk));
    }

    // Free count must remain completely unaffected
    ASSERT_EQ(bitmap.count_free_blocks(), 16365ULL);
}

// 10. Full bitmap exhaustion and recovery
void test_full_bitmap_exhaustion() {
    auto file = create_formatted_disk("build/test_bm_exhaustion.img");
    BlockDevice dev(file->path());
    BlockBitmap bitmap(dev);

    // Allocate all 16,365 available data blocks
    for (std::uint64_t i = 0; i < 16365; ++i) {
        const std::uint64_t blk = bitmap.allocate();
        ASSERT_EQ(blk, 19ULL + i);
    }

    ASSERT_EQ(bitmap.count_free_blocks(), 0ULL);

    // When fully exhausted, allocate must fail with NoFreeBlocksError
    ASSERT_THROWS(bitmap.allocate(), NoFreeBlocksError);

    // Free block 500
    bitmap.free(500);
    ASSERT_EQ(bitmap.count_free_blocks(), 1ULL);

    // Allocating should succeed and return block 500
    const std::uint64_t reused = bitmap.allocate();
    ASSERT_EQ(reused, 500ULL);
    ASSERT_EQ(bitmap.count_free_blocks(), 0ULL);

    // Exhausted again
    ASSERT_THROWS(bitmap.allocate(), NoFreeBlocksError);
}

// 11. Correct free-block count
void test_correct_free_block_count() {
    auto file = create_formatted_disk("build/test_bm_count.img");
    BlockDevice dev(file->path());
    BlockBitmap bitmap(dev);

    ASSERT_EQ(bitmap.count_free_blocks(), 16365ULL);

    std::vector<std::uint64_t> allocated;
    for (int i = 0; i < 50; ++i) {
        allocated.push_back(bitmap.allocate());
    }
    ASSERT_EQ(bitmap.count_free_blocks(), 16365ULL - 50ULL);

    // Free every second block (25 blocks)
    for (std::size_t i = 0; i < allocated.size(); i += 2) {
        bitmap.free(allocated[i]);
    }
    ASSERT_EQ(bitmap.count_free_blocks(), 16365ULL - 25ULL);
}

// 12. Persistence after destroying and recreating BlockBitmap object
void test_persistence_across_instances_and_reopen() {
    auto file = create_formatted_disk("build/test_bm_persist.img");

    // Session 1: perform operations
    {
        BlockDevice dev(file->path());
        BlockBitmap bitmap(dev);

        bitmap.allocate(); // 19
        bitmap.allocate(); // 20
        bitmap.allocate(); // 21
        bitmap.allocate(); // 22

        bitmap.free(20); // free 20
        ASSERT_EQ(bitmap.count_free_blocks(), 16362ULL);
        dev.close();
    }

    // Session 2: reopen and verify persisted bitmap on disk
    {
        BlockDevice dev(file->path());
        BlockBitmap bitmap(dev);

        ASSERT_TRUE(bitmap.is_allocated(19));
        ASSERT_FALSE(bitmap.is_allocated(20));
        ASSERT_TRUE(bitmap.is_allocated(21));
        ASSERT_TRUE(bitmap.is_allocated(22));
        ASSERT_EQ(bitmap.count_free_blocks(), 16362ULL);

        // Next allocation reallocates block 20
        std::uint64_t reallocated = bitmap.allocate();
        ASSERT_EQ(reallocated, 20ULL);
        ASSERT_TRUE(bitmap.is_allocated(20));
        ASSERT_EQ(bitmap.count_free_blocks(), 16361ULL);
    }
}

// 13. Bitmap initialize() resets bitmap state consistently
void test_bitmap_initialize() {
    auto file = create_formatted_disk("build/test_bm_init.img");

    {
        BlockDevice dev(file->path());
        BlockBitmap bitmap(dev);

        // Allocate several blocks
        for (int i = 0; i < 10; ++i) {
            bitmap.allocate();
        }
        ASSERT_EQ(bitmap.count_free_blocks(), 16355ULL);

        // Reset via initialize()
        bitmap.initialize();
        ASSERT_EQ(bitmap.count_free_blocks(), 16365ULL);
        for (std::uint64_t blk = 0; blk < 19; ++blk) {
            ASSERT_TRUE(bitmap.is_allocated(blk));
        }
        for (std::uint64_t blk = 19; blk < 30; ++blk) {
            ASSERT_FALSE(bitmap.is_allocated(blk));
        }
    }

    // Verify persisted across reload
    {
        BlockDevice dev(file->path());
        BlockBitmap bitmap(dev);
        ASSERT_EQ(bitmap.count_free_blocks(), 16365ULL);
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
    std::cout << "              Filler FileSystem: BlockBitmap Test Suite               \n";
    std::cout << "======================================================================\n";

    TestRunner runner;

    runner.run("1. Initial bitmap state after formatting", test_initial_bitmap_state);
    runner.run("2. First allocation returns the first available data block", test_first_allocation);
    runner.run("3. Multiple consecutive allocations", test_multiple_consecutive_allocations);
    runner.run("4. Correct allocation status", test_correct_allocation_status);
    runner.run("5. Successful deallocation", test_successful_deallocation);
    runner.run("6. Reallocation of a previously freed block", test_reallocation_of_freed_block);
    runner.run("7. Double-free rejection", test_double_free_rejection);
    runner.run("8. Invalid block ID handling", test_invalid_block_id_handling);
    runner.run("9. Protection of metadata blocks", test_protection_of_metadata_blocks);
    runner.run("10. Full bitmap exhaustion and recovery", test_full_bitmap_exhaustion);
    runner.run("11. Correct free-block count", test_correct_free_block_count);
    runner.run("12. Persistence after destroying and recreating BlockBitmap", test_persistence_across_instances_and_reopen);
    runner.run("13. Bitmap initialize() resets bitmap state consistently", test_bitmap_initialize);

    std::cout << "======================================================================\n";
    std::cout << "Test Summary: " << runner.passed << " / " << runner.total << " passed ("
              << runner.failed << " failed)\n";
    std::cout << "======================================================================\n";

    return (runner.failed == 0) ? 0 : 1;
}
