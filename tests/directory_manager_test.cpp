#include "fs/BlockBitmap.hpp"
#include "fs/DirectoryManager.hpp"
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

using filler::fs::DirectoryEntry;
using filler::fs::DirectoryManager;
using filler::fs::DirectoryManagerError;
using filler::fs::EntryNotFoundError;
using filler::fs::FileType;
using filler::fs::Formatter;
using filler::fs::Inode;
using filler::fs::InodeManager;
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
// Test Implementations
// ============================================================================

// 1. Root directory initialization
void test_root_directory_initialization() {
    auto file = create_formatted_disk("build/test_dm_init_root.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);

    dm.initialize_root();

    Inode root = dm.inode_manager().get_inode(Superblock::DEFAULT_ROOT_INODE);
    ASSERT_TRUE(root.is_directory());
    ASSERT_EQ(root.inode_id, Superblock::DEFAULT_ROOT_INODE);
    ASSERT_EQ(root.permissions, 0755U);
    ASSERT_TRUE(root.blocks_count >= 1);
    ASSERT_EQ(root.direct_blocks[0], 19ULL); // First data block
    ASSERT_TRUE(root.size_bytes > 0);
}

// 2. Root '.' and '..' entries
void test_root_dot_and_dotdot_entries() {
    auto file = create_formatted_disk("build/test_dm_root_dot.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);
    dm.initialize_root();

    // Verify lookup of . and .. in root
    ASSERT_EQ(dm.lookup(1, "."), 1ULL);
    ASSERT_EQ(dm.lookup(1, ".."), 1ULL);

    // Verify list of root contains exactly . and ..
    std::vector<DirectoryEntry> entries = dm.list(1);
    ASSERT_EQ(entries.size(), 2ULL);
    ASSERT_EQ(entries[0].name, ".");
    ASSERT_EQ(entries[0].inode_id, 1ULL);
    ASSERT_TRUE(entries[0].type == FileType::DIRECTORY);

    ASSERT_EQ(entries[1].name, "..");
    ASSERT_EQ(entries[1].inode_id, 1ULL);
    ASSERT_TRUE(entries[1].type == FileType::DIRECTORY);
}

// 3. Creating a directory
void test_creating_a_directory() {
    auto file = create_formatted_disk("build/test_dm_mkdir.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);
    dm.initialize_root();

    const std::uint64_t home_ino = dm.mkdir(1, "home");
    ASSERT_EQ(home_ino, 2ULL); // First available user inode

    // Verify lookup in root
    ASSERT_EQ(dm.lookup(1, "home"), 2ULL);

    // Verify child has . (pointing to 2) and .. (pointing to 1)
    std::vector<DirectoryEntry> child_entries = dm.list(2);
    ASSERT_EQ(child_entries.size(), 2ULL);
    ASSERT_EQ(child_entries[0].name, ".");
    ASSERT_EQ(child_entries[0].inode_id, 2ULL);
    ASSERT_EQ(child_entries[1].name, "..");
    ASSERT_EQ(child_entries[1].inode_id, 1ULL);

    // Verify root now has 3 entries: ., .., home
    std::vector<DirectoryEntry> root_entries = dm.list(1);
    ASSERT_EQ(root_entries.size(), 3ULL);
    ASSERT_EQ(root_entries[2].name, "home");
    ASSERT_EQ(root_entries[2].inode_id, 2ULL);
}

// 4. Creating multiple directories
void test_creating_multiple_directories() {
    auto file = create_formatted_disk("build/test_dm_multi_mkdir.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);
    dm.initialize_root();

    std::vector<std::string> names = {"bin", "etc", "home", "usr", "var", "tmp"};
    for (const auto& name : names) {
        dm.mkdir(1, name);
    }

    std::vector<DirectoryEntry> entries = dm.list(1);
    ASSERT_EQ(entries.size(), 2ULL + names.size());

    for (const auto& name : names) {
        std::uint64_t ino = dm.lookup(1, name);
        ASSERT_TRUE(ino >= 2);
    }
}

// 5. Looking up directory entries
void test_looking_up_directory_entries() {
    auto file = create_formatted_disk("build/test_dm_lookup.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);
    dm.initialize_root();

    const std::uint64_t etc_ino = dm.mkdir(1, "etc");
    ASSERT_EQ(dm.lookup(1, "etc"), etc_ino);

    // Nonexistent lookup throws EntryNotFoundError / DirectoryManagerError
    ASSERT_THROWS(dm.lookup(1, "nonexistent"), EntryNotFoundError);
    ASSERT_THROWS(dm.lookup(1, ""), DirectoryManagerError);
}

// 6. Listing directory contents
void test_listing_directory_contents() {
    auto file = create_formatted_disk("build/test_dm_list.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);
    dm.initialize_root();

    dm.mkdir(1, "alpha");
    dm.mkdir(1, "beta");

    std::vector<DirectoryEntry> entries = dm.list(1);
    ASSERT_EQ(entries.size(), 4ULL);
    ASSERT_EQ(entries[0].name, ".");
    ASSERT_EQ(entries[1].name, "..");
    ASSERT_EQ(entries[2].name, "alpha");
    ASSERT_EQ(entries[3].name, "beta");
}

// 7. Duplicate filename rejection
void test_duplicate_filename_rejection() {
    auto file = create_formatted_disk("build/test_dm_duplicate.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);
    dm.initialize_root();

    dm.mkdir(1, "docs");
    ASSERT_THROWS(dm.mkdir(1, "docs"), DirectoryManagerError);
    ASSERT_THROWS(dm.mkdir(1, "."), DirectoryManagerError);
    ASSERT_THROWS(dm.mkdir(1, ".."), DirectoryManagerError);
}

// 8. Invalid parent inode handling
void test_invalid_parent_inode_handling() {
    auto file = create_formatted_disk("build/test_dm_invalid_parent.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);
    dm.initialize_root();

    // Out of range parent
    ASSERT_THROWS(dm.mkdir(256, "foo"), DirectoryManagerError);
    ASSERT_THROWS(dm.lookup(256, "foo"), DirectoryManagerError);
    ASSERT_THROWS(dm.list(256), DirectoryManagerError);

    // Unallocated parent
    ASSERT_THROWS(dm.mkdir(50, "foo"), DirectoryManagerError);
    ASSERT_THROWS(dm.lookup(50, "foo"), DirectoryManagerError);
    ASSERT_THROWS(dm.list(50), DirectoryManagerError);
}

// 9. Rejecting mkdir inside a regular file
void test_rejecting_mkdir_inside_regular_file() {
    auto file = create_formatted_disk("build/test_dm_inside_file.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);
    dm.initialize_root();

    // Allocate a regular file inode
    const std::uint64_t file_ino = dm.inode_manager().allocate_inode(FileType::REGULAR, 0644);

    ASSERT_THROWS(dm.mkdir(file_ino, "subfolder"), DirectoryManagerError);
    ASSERT_THROWS(dm.lookup(file_ino, "subfolder"), DirectoryManagerError);
    ASSERT_THROWS(dm.list(file_ino), DirectoryManagerError);
}

// 10. Nested directory creation and hierarchy traversal
void test_nested_directory_creation() {
    auto file = create_formatted_disk("build/test_dm_nested.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);
    dm.initialize_root();

    // Create /a/b/c
    const std::uint64_t a = dm.mkdir(1, "a");
    const std::uint64_t b = dm.mkdir(a, "b");
    const std::uint64_t c = dm.mkdir(b, "c");

    ASSERT_EQ(dm.lookup(1, "a"), a);
    ASSERT_EQ(dm.lookup(a, "b"), b);
    ASSERT_EQ(dm.lookup(b, "c"), c);

    // Verify parent relationships
    ASSERT_EQ(dm.lookup(c, ".."), b);
    ASSERT_EQ(dm.lookup(b, ".."), a);
    ASSERT_EQ(dm.lookup(a, ".."), 1ULL);
    ASSERT_EQ(dm.lookup(1, ".."), 1ULL);
}

// 11. Removing an empty directory
void test_removing_empty_directory() {
    auto file = create_formatted_disk("build/test_dm_remove.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);
    dm.initialize_root();

    const std::uint64_t temp_ino = dm.mkdir(1, "temp");
    ASSERT_EQ(dm.lookup(1, "temp"), temp_ino);
    ASSERT_TRUE(dm.inode_manager().is_inode_allocated(temp_ino));

    dm.remove_directory(1, "temp");

    // Lookup must fail
    ASSERT_THROWS(dm.lookup(1, "temp"), EntryNotFoundError);

    // Inode must be deallocated
    ASSERT_FALSE(dm.inode_manager().is_inode_allocated(temp_ino));

    // Root directory entries must be back to 2 (., ..)
    ASSERT_EQ(dm.list(1).size(), 2ULL);
}

// 12. Rejecting removal of non-empty directories
void test_rejecting_removal_of_non_empty_directory() {
    auto file = create_formatted_disk("build/test_dm_remove_nonempty.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);
    dm.initialize_root();

    const std::uint64_t parent_dir = dm.mkdir(1, "parent");
    dm.mkdir(parent_dir, "child");

    // Attempting to remove parent must fail
    ASSERT_THROWS(dm.remove_directory(1, "parent"), DirectoryManagerError);

    // Both directories must still exist
    ASSERT_EQ(dm.lookup(1, "parent"), parent_dir);
    ASSERT_EQ(dm.list(parent_dir).size(), 3ULL);
}

// 13. Rejecting root deletion and special entry deletion
void test_rejecting_root_deletion() {
    auto file = create_formatted_disk("build/test_dm_protect_root.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);
    dm.initialize_root();

    ASSERT_THROWS(dm.remove_directory(1, "."), DirectoryManagerError);
    ASSERT_THROWS(dm.remove_directory(1, ".."), DirectoryManagerError);
}

// 14. Persistence after reopening disk image
void test_persistence_across_reopen() {
    auto file = create_formatted_disk("build/test_dm_persist.img");

    std::uint64_t projects_ino = 0;
    std::uint64_t filler_ino = 0;

    // Session 1: Create structure /projects/filler
    {
        BlockDevice dev(file->path());
        DirectoryManager dm(dev);
        dm.initialize_root();

        projects_ino = dm.mkdir(1, "projects");
        filler_ino = dm.mkdir(projects_ino, "filler");
        dev.close();
    }

    // Session 2: Reopen and verify persisted hierarchy
    {
        BlockDevice dev(file->path());
        DirectoryManager dm(dev);

        ASSERT_EQ(dm.lookup(1, "projects"), projects_ino);
        ASSERT_EQ(dm.lookup(projects_ino, "filler"), filler_ino);

        std::vector<DirectoryEntry> proj_entries = dm.list(projects_ino);
        ASSERT_EQ(proj_entries.size(), 3ULL);
        ASSERT_EQ(proj_entries[0].name, ".");
        ASSERT_EQ(proj_entries[1].name, "..");
        ASSERT_EQ(proj_entries[2].name, "filler");

        std::vector<DirectoryEntry> filler_entries = dm.list(filler_ino);
        ASSERT_EQ(filler_entries.size(), 2ULL);
        ASSERT_EQ(filler_entries[0].name, ".");
        ASSERT_EQ(filler_entries[1].name, "..");
    }
}

// 15. Block and inode allocation/deallocation correctness
void test_allocation_and_deallocation_counts() {
    auto file = create_formatted_disk("build/test_dm_counts.img");
    BlockDevice dev(file->path());
    DirectoryManager dm(dev);
    dm.initialize_root();

    const std::uint64_t initial_free_inodes = dm.inode_manager().count_free_inodes();
    const std::uint64_t initial_free_blocks = dm.block_bitmap().count_free_blocks();

    // mkdir allocates 1 inode and 1 data block
    const std::uint64_t d = dm.mkdir(1, "sandbox");
    ASSERT_EQ(dm.inode_manager().count_free_inodes(), initial_free_inodes - 1ULL);
    ASSERT_EQ(dm.block_bitmap().count_free_blocks(), initial_free_blocks - 1ULL);

    // remove_directory frees 1 inode and 1 data block
    dm.remove_directory(1, "sandbox");
    ASSERT_EQ(dm.inode_manager().count_free_inodes(), initial_free_inodes);
    ASSERT_EQ(dm.block_bitmap().count_free_blocks(), initial_free_blocks);

    // Can re-mkdir reusing the freed inode and block
    const std::uint64_t d2 = dm.mkdir(1, "sandbox2");
    ASSERT_EQ(d2, d); // Reuses freed inode ID
    ASSERT_EQ(dm.inode_manager().count_free_inodes(), initial_free_inodes - 1ULL);
    ASSERT_EQ(dm.block_bitmap().count_free_blocks(), initial_free_blocks - 1ULL);
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
    std::cout << "             Filler FileSystem: DirectoryManager Test Suite           \n";
    std::cout << "======================================================================\n";

    TestRunner runner;

    runner.run("1. Root directory initialization", test_root_directory_initialization);
    runner.run("2. Root '.' and '..' entries", test_root_dot_and_dotdot_entries);
    runner.run("3. Creating a directory", test_creating_a_directory);
    runner.run("4. Creating multiple directories", test_creating_multiple_directories);
    runner.run("5. Looking up directory entries", test_looking_up_directory_entries);
    runner.run("6. Listing directory contents", test_listing_directory_contents);
    runner.run("7. Duplicate filename rejection", test_duplicate_filename_rejection);
    runner.run("8. Invalid parent inode handling", test_invalid_parent_inode_handling);
    runner.run("9. Rejecting mkdir inside a regular file", test_rejecting_mkdir_inside_regular_file);
    runner.run("10. Nested directory creation and hierarchy traversal", test_nested_directory_creation);
    runner.run("11. Removing an empty directory", test_removing_empty_directory);
    runner.run("12. Rejecting removal of non-empty directories", test_rejecting_removal_of_non_empty_directory);
    runner.run("13. Rejecting root deletion and special entry deletion", test_rejecting_root_deletion);
    runner.run("14. Persistence after reopening disk image", test_persistence_across_reopen);
    runner.run("15. Block and inode allocation/deallocation correctness", test_allocation_and_deallocation_counts);

    std::cout << "======================================================================\n";
    std::cout << "Test Summary: " << runner.passed << " / " << runner.total << " passed ("
              << runner.failed << " failed)\n";
    std::cout << "======================================================================\n";

    return (runner.failed == 0) ? 0 : 1;
}

