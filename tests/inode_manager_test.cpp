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

using filler::fs::FileType;
using filler::fs::Formatter;
using filler::fs::Inode;
using filler::fs::InodeManager;
using filler::fs::InodeManagerError;
using filler::fs::InodeSerializationError;
using filler::fs::NoFreeInodesError;
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

std::unique_ptr<ScopedTestFile> create_formatted_disk(const std::string& filename) {
    auto file = std::make_unique<ScopedTestFile>(filename);
    file->create(Formatter::DEFAULT_DISK_SIZE, 0);
    BlockDevice dev(file->path());
    Formatter::format(dev);
    return file;
}

// ============================================================================
// Tests
// ============================================================================

// 1. Exact 256-byte Inode serialization & deserialization round trip
void test_exact_256_byte_serialization() {
    Inode orig;
    orig.inode_id = 42;
    orig.type = FileType::REGULAR;
    orig.permissions = 0644;
    orig.uid = 1000;
    orig.gid = 1000;
    orig.size_bytes = 1234567;
    orig.blocks_count = 3;
    orig.atime_sec = 1700000000;
    orig.mtime_sec = 1700000100;
    orig.ctime_sec = 1700000200;
    orig.direct_blocks[0] = 19;
    orig.direct_blocks[1] = 20;
    orig.direct_blocks[2] = 21;
    orig.single_indirect = 99;
    orig.double_indirect = 100;

    std::vector<std::uint8_t> buffer(Inode::INODE_SIZE);
    orig.serialize(buffer.data(), buffer.size());

    // Verify buffer size is exactly 256
    ASSERT_EQ(buffer.size(), 256ULL);

    Inode deserialized = Inode::deserialize(buffer.data(), buffer.size());
    ASSERT_TRUE(orig == deserialized);

    // Reject undersized buffers
    std::vector<std::uint8_t> small_buffer(255);
    ASSERT_THROWS(orig.serialize(small_buffer.data(), small_buffer.size()), InodeSerializationError);
    ASSERT_THROWS(orig.serialize(nullptr, 256), InodeSerializationError);
    ASSERT_THROWS(Inode::deserialize(small_buffer.data(), small_buffer.size()), InodeSerializationError);
    ASSERT_THROWS(Inode::deserialize(nullptr, 256), InodeSerializationError);
}

// 2. Allocation of the first available inode returns inode 2
void test_allocation_of_first_available_inode() {
    auto file = create_formatted_disk("build/test_im_first_alloc.img");
    BlockDevice dev(file->path());
    InodeManager im(dev);

    // On fresh disk, inodes 0 (reserved) and 1 (root) are allocated.
    // Free inodes must initially be 254.
    ASSERT_EQ(im.count_free_inodes(), 254ULL);
    ASSERT_TRUE(im.is_inode_allocated(0));
    ASSERT_TRUE(im.is_inode_allocated(1));
    ASSERT_FALSE(im.is_inode_allocated(2));

    const std::uint64_t ino = im.allocate_inode(FileType::REGULAR, 0644);
    ASSERT_EQ(ino, 2ULL);
    ASSERT_TRUE(im.is_inode_allocated(2));
    ASSERT_EQ(im.count_free_inodes(), 253ULL);
}

// 3. Multiple consecutive allocations
void test_multiple_consecutive_allocations() {
    auto file = create_formatted_disk("build/test_im_multi_alloc.img");
    BlockDevice dev(file->path());
    InodeManager im(dev);

    for (std::uint64_t i = 0; i < 20; ++i) {
        const std::uint64_t ino = im.allocate_inode(FileType::REGULAR, 0644);
        ASSERT_EQ(ino, 2ULL + i);
        ASSERT_TRUE(im.is_inode_allocated(ino));
    }

    ASSERT_EQ(im.count_free_inodes(), 254ULL - 20ULL);
}

// 4. Reading inode metadata (get_inode)
void test_reading_inode_metadata() {
    auto file = create_formatted_disk("build/test_im_get_inode.img");
    BlockDevice dev(file->path());
    InodeManager im(dev);

    const std::uint64_t ino = im.allocate_inode(FileType::DIRECTORY, 0755);
    Inode node = im.get_inode(ino);

    ASSERT_EQ(node.inode_id, ino);
    ASSERT_TRUE(node.is_directory());
    ASSERT_FALSE(node.is_regular_file());
    ASSERT_EQ(node.permissions, 0755U);
    ASSERT_EQ(node.size_bytes, 0ULL);
    ASSERT_EQ(node.blocks_count, 0ULL);
    ASSERT_TRUE(node.atime_sec > 0);
    ASSERT_TRUE(node.mtime_sec > 0);
    ASSERT_TRUE(node.ctime_sec > 0);
}

// 5. Updating inode metadata (update_inode)
void test_updating_inode_metadata() {
    auto file = create_formatted_disk("build/test_im_update_inode.img");
    BlockDevice dev(file->path());
    InodeManager im(dev);

    const std::uint64_t ino = im.allocate_inode(FileType::REGULAR, 0644);

    Inode modified = im.get_inode(ino);
    modified.size_bytes = 8192;
    modified.blocks_count = 2;
    modified.direct_blocks[0] = 19;
    modified.direct_blocks[1] = 20;
    modified.permissions = 0600;

    im.update_inode(ino, modified);

    Inode read_back = im.get_inode(ino);
    ASSERT_EQ(read_back.size_bytes, 8192ULL);
    ASSERT_EQ(read_back.blocks_count, 2ULL);
    ASSERT_EQ(read_back.direct_blocks[0], 19ULL);
    ASSERT_EQ(read_back.direct_blocks[1], 20ULL);
    ASSERT_EQ(read_back.permissions, 0600U);
}

// 6. Neighboring inode isolation in the same disk block
void test_neighboring_inode_isolation() {
    auto file = create_formatted_disk("build/test_im_neighbor_isolation.img");
    BlockDevice dev(file->path());
    InodeManager im(dev);

    // Inodes 2 and 3 reside in the same disk block (block 3)
    const std::uint64_t ino2 = im.allocate_inode(FileType::REGULAR, 0644);
    const std::uint64_t ino3 = im.allocate_inode(FileType::DIRECTORY, 0755);
    ASSERT_EQ(ino2, 2ULL);
    ASSERT_EQ(ino3, 3ULL);

    // Update ino2
    Inode node2 = im.get_inode(ino2);
    node2.size_bytes = 4096;
    node2.direct_blocks[0] = 50;
    im.update_inode(ino2, node2);

    // Ensure ino3 is completely untouched
    Inode node3 = im.get_inode(ino3);
    ASSERT_EQ(node3.inode_id, 3ULL);
    ASSERT_TRUE(node3.is_directory());
    ASSERT_EQ(node3.permissions, 0755U);
    ASSERT_EQ(node3.size_bytes, 0ULL);
}

// 7. Freeing an inode and reallocating it
void test_freeing_and_reallocating_inode() {
    auto file = create_formatted_disk("build/test_im_free_realloc.img");
    BlockDevice dev(file->path());
    InodeManager im(dev);

    std::uint64_t i1 = im.allocate_inode(); // 2
    std::uint64_t i2 = im.allocate_inode(); // 3
    std::uint64_t i3 = im.allocate_inode(); // 4

    ASSERT_EQ(i1, 2ULL);
    ASSERT_EQ(i2, 3ULL);
    ASSERT_EQ(i3, 4ULL);
    ASSERT_EQ(im.count_free_inodes(), 251ULL);

    im.free_inode(i2); // Free inode 3
    ASSERT_FALSE(im.is_inode_allocated(3));
    ASSERT_EQ(im.count_free_inodes(), 252ULL);

    // Getting freed inode must throw
    ASSERT_THROWS(im.get_inode(3), InodeManagerError);

    // Next allocate reuses freed inode 3
    std::uint64_t reallocated = im.allocate_inode();
    ASSERT_EQ(reallocated, 3ULL);
    ASSERT_TRUE(im.is_inode_allocated(3));
    ASSERT_EQ(im.count_free_inodes(), 251ULL);

    // Subsequent allocate continues at 5
    std::uint64_t i4 = im.allocate_inode();
    ASSERT_EQ(i4, 5ULL);
}

// 8. Double-free rejection
void test_double_free_rejection() {
    auto file = create_formatted_disk("build/test_im_double_free.img");
    BlockDevice dev(file->path());
    InodeManager im(dev);

    std::uint64_t ino = im.allocate_inode();
    im.free_inode(ino);

    // Freeing again must throw
    ASSERT_THROWS(im.free_inode(ino), InodeManagerError);

    // Freeing an unallocated inode must throw
    ASSERT_THROWS(im.free_inode(100), InodeManagerError);
}

// 9. Invalid inode ID handling
void test_invalid_inode_id_handling() {
    auto file = create_formatted_disk("build/test_im_invalid_id.img");
    BlockDevice dev(file->path());
    InodeManager im(dev);

    ASSERT_THROWS(im.get_inode(256), InodeManagerError);
    ASSERT_THROWS(im.get_inode(99999), InodeManagerError);

    Inode dummy;
    ASSERT_THROWS(im.update_inode(256, dummy), InodeManagerError);
    ASSERT_THROWS(im.update_inode(99999, dummy), InodeManagerError);

    ASSERT_THROWS(im.free_inode(256), InodeManagerError);
    ASSERT_THROWS(im.free_inode(99999), InodeManagerError);

    ASSERT_THROWS(im.is_inode_allocated(256), InodeManagerError);
    ASSERT_THROWS(im.is_inode_allocated(99999), InodeManagerError);
}

// 10. Protection of reserved inodes (inode 0 and root inode 1)
void test_protection_of_reserved_inodes() {
    auto file = create_formatted_disk("build/test_im_protect_reserved.img");
    BlockDevice dev(file->path());
    InodeManager im(dev);

    // Inode 0 (reserved) and Inode 1 (root) cannot be freed
    ASSERT_TRUE(im.is_inode_allocated(0));
    ASSERT_TRUE(im.is_inode_allocated(1));

    ASSERT_THROWS(im.free_inode(0), InodeManagerError);
    ASSERT_THROWS(im.free_inode(1), InodeManagerError);

    // Inode allocation status must remain true
    ASSERT_TRUE(im.is_inode_allocated(0));
    ASSERT_TRUE(im.is_inode_allocated(1));
}

// 11. Inode exhaustion and recovery
void test_inode_exhaustion() {
    auto file = create_formatted_disk("build/test_im_exhaustion.img");
    BlockDevice dev(file->path());
    InodeManager im(dev);

    // Allocate all 254 free inodes (2 to 255)
    for (std::uint64_t i = 0; i < 254; ++i) {
        const std::uint64_t ino = im.allocate_inode();
        ASSERT_EQ(ino, 2ULL + i);
    }

    ASSERT_EQ(im.count_free_inodes(), 0ULL);

    // Next allocate must throw NoFreeInodesError
    ASSERT_THROWS(im.allocate_inode(), NoFreeInodesError);

    // Free inode 100
    im.free_inode(100);
    ASSERT_EQ(im.count_free_inodes(), 1ULL);

    // Allocating should succeed and return inode 100
    const std::uint64_t reused = im.allocate_inode();
    ASSERT_EQ(reused, 100ULL);
    ASSERT_EQ(im.count_free_inodes(), 0ULL);

    // Exhausted again
    ASSERT_THROWS(im.allocate_inode(), NoFreeInodesError);
}

// 12. Persistence after reopening disk image
void test_persistence_across_reopen() {
    auto file = create_formatted_disk("build/test_im_persist.img");

    // Session 1: allocate and update
    {
        BlockDevice dev(file->path());
        InodeManager im(dev);

        std::uint64_t ino = im.allocate_inode(FileType::REGULAR, 0640);
        ASSERT_EQ(ino, 2ULL);

        Inode node = im.get_inode(ino);
        node.size_bytes = 65536;
        node.blocks_count = 16;
        node.direct_blocks[0] = 100;
        node.direct_blocks[15] = 115;
        im.update_inode(ino, node);

        dev.close();
    }

    // Session 2: reopen and verify persisted inode
    {
        BlockDevice dev(file->path());
        InodeManager im(dev);

        ASSERT_TRUE(im.is_inode_allocated(2));
        Inode node = im.get_inode(2);

        ASSERT_EQ(node.inode_id, 2ULL);
        ASSERT_TRUE(node.is_regular_file());
        ASSERT_EQ(node.permissions, 0640U);
        ASSERT_EQ(node.size_bytes, 65536ULL);
        ASSERT_EQ(node.blocks_count, 16ULL);
        ASSERT_EQ(node.direct_blocks[0], 100ULL);
        ASSERT_EQ(node.direct_blocks[15], 115ULL);
        ASSERT_EQ(im.count_free_inodes(), 253ULL);
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
    std::cout << "              Filler FileSystem: InodeManager Test Suite              \n";
    std::cout << "======================================================================\n";

    TestRunner runner;

    runner.run("1. Exact 256-byte Inode serialization & deserialization", test_exact_256_byte_serialization);
    runner.run("2. Allocation of the first available inode returns inode 2", test_allocation_of_first_available_inode);
    runner.run("3. Multiple consecutive allocations", test_multiple_consecutive_allocations);
    runner.run("4. Reading inode metadata (get_inode)", test_reading_inode_metadata);
    runner.run("5. Updating inode metadata (update_inode)", test_updating_inode_metadata);
    runner.run("6. Neighboring inode isolation in the same disk block", test_neighboring_inode_isolation);
    runner.run("7. Freeing an inode and reallocating it", test_freeing_and_reallocating_inode);
    runner.run("8. Double-free rejection", test_double_free_rejection);
    runner.run("9. Invalid inode ID handling", test_invalid_inode_id_handling);
    runner.run("10. Protection of reserved inodes (inode 0 and root inode 1)", test_protection_of_reserved_inodes);
    runner.run("11. Inode exhaustion and recovery", test_inode_exhaustion);
    runner.run("12. Persistence after reopening disk image", test_persistence_across_reopen);

    std::cout << "======================================================================\n";
    std::cout << "Test Summary: " << runner.passed << " / " << runner.total << " passed ("
              << runner.failed << " failed)\n";
    std::cout << "======================================================================\n";

    return (runner.failed == 0) ? 0 : 1;
}

