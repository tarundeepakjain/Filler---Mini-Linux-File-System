#include "cli/Shell.hpp"
#include "fs/BlockBitmap.hpp"
#include "fs/DirectoryManager.hpp"
#include "fs/FileManager.hpp"
#include "fs/Formatter.hpp"
#include "fs/Inode.hpp"
#include "fs/InodeManager.hpp"
#include "fs/Superblock.hpp"
#include "storage/BlockDevice.hpp"

// POSIX and system headers
#include <fcntl.h>
#include <unistd.h>

// C++ standard library
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using filler::cli::Shell;
using filler::fs::BlockBitmap;
using filler::fs::DirectoryEntry;
using filler::fs::DirectoryManager;
using filler::fs::FileManager;
using filler::fs::FileType;
using filler::fs::Formatter;
using filler::fs::Inode;
using filler::fs::InodeManager;
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

// ============================================================================
// Disposable Test Image Management (RAII)
// ============================================================================

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

private:
    std::string path_;
};

// ============================================================================
// End-to-End Test Scenarios
// ============================================================================

// Scenario A: Basic Filesystem
// 1. Create fresh disk image
// 2. Format it
// 3. Start Filler
// 4. List root directory
// 5. Verify filesystem metadata
void test_scenario_a_basic_filesystem() {
    ScopedTestFile img("build/test_e2e_scenario_a.img");
    Shell shell(img.path());

    // Initially unformatted
    ASSERT_FALSE(shell.is_formatted());

    std::ostringstream out;
    ASSERT_TRUE(shell.execute_command("format", out));
    ASSERT_TRUE(out.str().find("successfully formatted") != std::string::npos);
    ASSERT_TRUE(shell.is_formatted());
    ASSERT_EQ(shell.current_path(), "/");
    ASSERT_EQ(shell.current_dir_inode(), Superblock::DEFAULT_ROOT_INODE);

    // List root directory
    out.str("");
    ASSERT_TRUE(shell.execute_command("ls", out));
    std::string ls_output = out.str();
    ASSERT_TRUE(ls_output.find("[DIR]  .") != std::string::npos);
    ASSERT_TRUE(ls_output.find("[DIR]  ..") != std::string::npos);

    // Verify filesystem metadata directly from raw disk device
    BlockDevice dev(img.path());
    Superblock sb = Superblock::read(dev);
    ASSERT_EQ(sb.magic, Superblock::MAGIC);
    ASSERT_EQ(sb.version, Superblock::FORMAT_VERSION);
    ASSERT_EQ(sb.block_size, Superblock::DEFAULT_BLOCK_SIZE);
    ASSERT_EQ(sb.inode_size, Superblock::DEFAULT_INODE_SIZE);
    ASSERT_EQ(sb.total_blocks, 16384ULL);
    ASSERT_EQ(sb.total_inodes, 256ULL);
    ASSERT_EQ(sb.root_inode, 1ULL);
    ASSERT_EQ(sb.block_bitmap_start, 1ULL);
    ASSERT_EQ(sb.block_bitmap_blocks, 1ULL);
    ASSERT_EQ(sb.inode_bitmap_start, 2ULL);
    ASSERT_EQ(sb.inode_bitmap_blocks, 1ULL);
    ASSERT_EQ(sb.inode_table_start, 3ULL);
    ASSERT_EQ(sb.inode_table_blocks, 16ULL);
    ASSERT_EQ(sb.data_region_start, 19ULL);
    ASSERT_EQ(sb.data_region_blocks, 16365ULL);

    // Root directory occupies 1 block (block 19), so 16364 free data blocks
    ASSERT_EQ(sb.free_blocks, 16364ULL);
    // Inode 0 reserved, inode 1 root directory allocated, so 254 free inodes
    ASSERT_EQ(sb.free_inodes, 254ULL);

    // Check root inode metadata
    InodeManager im(dev, sb);
    Inode root = im.get_inode(1);
    ASSERT_TRUE(root.is_directory());
    ASSERT_EQ(root.inode_id, 1ULL);
    ASSERT_EQ(root.blocks_count, 1ULL);
    ASSERT_EQ(root.direct_blocks[0], 19ULL);
}

// Scenario B: Directory Operations
// 1. Create multiple directories
// 2. Navigate between directories
// 3. Create nested directories
// 4. Test absolute and relative paths
// 5. Verify . and ..
// 6. Remove empty directories
// 7. Verify non-empty directories cannot be removed
void test_scenario_b_directory_operations() {
    ScopedTestFile img("build/test_e2e_scenario_b.img");
    Shell shell(img.path());
    std::ostringstream out;
    shell.format_filesystem(out);

    // 1. Create multiple directories
    ASSERT_TRUE(shell.execute_command("mkdir dir1", out));
    ASSERT_TRUE(shell.execute_command("mkdir dir2", out));
    ASSERT_TRUE(shell.execute_command("mkdir dir3", out));

    out.str("");
    ASSERT_TRUE(shell.execute_command("ls", out));
    std::string ls_root = out.str();
    ASSERT_TRUE(ls_root.find("[DIR]  dir1") != std::string::npos);
    ASSERT_TRUE(ls_root.find("[DIR]  dir2") != std::string::npos);
    ASSERT_TRUE(ls_root.find("[DIR]  dir3") != std::string::npos);

    // 2. Navigate between directories
    ASSERT_TRUE(shell.execute_command("cd dir1", out));
    ASSERT_EQ(shell.current_path(), "/dir1");

    // 3. Create nested directories: /dir1/sub1/leaf
    ASSERT_TRUE(shell.execute_command("mkdir sub1", out));
    ASSERT_TRUE(shell.execute_command("cd sub1", out));
    ASSERT_EQ(shell.current_path(), "/dir1/sub1");

    ASSERT_TRUE(shell.execute_command("mkdir leaf", out));
    ASSERT_TRUE(shell.execute_command("cd leaf", out));
    ASSERT_EQ(shell.current_path(), "/dir1/sub1/leaf");

    // 4. & 5. Test . and .. and relative / absolute paths
    ASSERT_TRUE(shell.execute_command("cd .", out));
    ASSERT_EQ(shell.current_path(), "/dir1/sub1/leaf");

    ASSERT_TRUE(shell.execute_command("cd ..", out));
    ASSERT_EQ(shell.current_path(), "/dir1/sub1");

    ASSERT_TRUE(shell.execute_command("cd ../..", out));
    ASSERT_EQ(shell.current_path(), "/");

    // cd with absolute path
    ASSERT_TRUE(shell.execute_command("cd /dir1/sub1/leaf", out));
    ASSERT_EQ(shell.current_path(), "/dir1/sub1/leaf");

    // cd to root with multiple .. from /
    ASSERT_TRUE(shell.execute_command("cd /../../..", out));
    ASSERT_EQ(shell.current_path(), "/");

    // 7. Verify non-empty directory cannot be removed
    out.str("");
    ASSERT_TRUE(shell.execute_command("rmdir dir1", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);

    // 6. Remove empty directories
    out.str("");
    ASSERT_TRUE(shell.execute_command("rmdir /dir1/sub1/leaf", out));
    ASSERT_TRUE(out.str().find("Error") == std::string::npos);

    out.str("");
    ASSERT_TRUE(shell.execute_command("rmdir /dir1/sub1", out));
    ASSERT_TRUE(out.str().find("Error") == std::string::npos);

    out.str("");
    ASSERT_TRUE(shell.execute_command("rmdir /dir1", out));
    ASSERT_TRUE(out.str().find("Error") == std::string::npos);

    // Root directory removal must be rejected
    out.str("");
    ASSERT_TRUE(shell.execute_command("rmdir /", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);

    out.str("");
    ASSERT_TRUE(shell.execute_command("rmdir .", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);
}

// Scenario C: File Operations
// 1. Create files
// 2. Write content
// 3. Read content
// 4. Append content
// 5. Overwrite partial content
// 6. Create files larger than one block
// 7. Truncate files
// 8. Remove files
void test_scenario_c_file_operations() {
    ScopedTestFile img("build/test_e2e_scenario_c.img");
    Shell shell(img.path());
    std::ostringstream out;
    shell.format_filesystem(out);

    // 1. Create empty file with touch
    ASSERT_TRUE(shell.execute_command("touch empty.txt", out));
    out.str("");
    ASSERT_TRUE(shell.execute_command("cat empty.txt", out));
    ASSERT_EQ(out.str(), "");

    out.str("");
    ASSERT_TRUE(shell.execute_command("stat empty.txt", out));
    ASSERT_TRUE(out.str().find("Size: 0 bytes") != std::string::npos);
    ASSERT_TRUE(out.str().find("Blocks: 0") != std::string::npos);

    // 2. Write content & 3. Read content
    out.str("");
    ASSERT_TRUE(shell.execute_command("write test.txt \"Hello Filler World!\"", out));
    ASSERT_TRUE(shell.execute_command("cat test.txt", out));
    ASSERT_EQ(out.str(), "Hello Filler World!\n");

    // 4. Append content
    out.str("");
    ASSERT_TRUE(shell.execute_command("append test.txt \" More data.\"", out));
    ASSERT_TRUE(shell.execute_command("cat test.txt", out));
    ASSERT_EQ(out.str(), "Hello Filler World! More data.\n");

    // 5. Overwrite partial content at offset 6: "Hello Planet World! More data."
    out.str("");
    ASSERT_TRUE(shell.execute_command("write test.txt 6 \"Planet\"", out));
    ASSERT_TRUE(shell.execute_command("cat test.txt", out));
    ASSERT_EQ(out.str(), "Hello Planet World! More data.\n");

    // 6. Create file larger than one block (> 4096 bytes)
    // We'll write 10,000 bytes spanning 3 blocks
    std::string large_content(10000, 'A');
    for (std::size_t i = 0; i < large_content.size(); ++i) {
        large_content[i] = static_cast<char>('A' + (i % 26));
    }
    std::uint64_t large_inode = shell.file_manager()->create(shell.current_dir_inode(), "large.bin");
    shell.file_manager()->write(large_inode, 0, large_content);

    // Read back through cat / read API and verify full byte match
    auto read_back = shell.file_manager()->read(large_inode, 0, 10000);
    ASSERT_EQ(read_back.size(), 10000ULL);
    ASSERT_EQ(std::memcmp(read_back.data(), large_content.data(), 10000), 0);

    out.str("");
    ASSERT_TRUE(shell.execute_command("stat large.bin", out));
    ASSERT_TRUE(out.str().find("Size: 10000 bytes") != std::string::npos);
    ASSERT_TRUE(out.str().find("Blocks: 3") != std::string::npos);

    // 7. Truncate files
    // Truncate from 10000 down to 2000 (shrinks to 1 block)
    ASSERT_TRUE(shell.execute_command("truncate large.bin 2000", out));
    out.str("");
    ASSERT_TRUE(shell.execute_command("stat large.bin", out));
    ASSERT_TRUE(out.str().find("Size: 2000 bytes") != std::string::npos);
    ASSERT_TRUE(out.str().find("Blocks: 1") != std::string::npos);

    auto truncated_back = shell.file_manager()->read(large_inode, 0, 2000);
    ASSERT_EQ(truncated_back.size(), 2000ULL);
    ASSERT_EQ(std::memcmp(truncated_back.data(), large_content.data(), 2000), 0);

    // Truncate to 0
    ASSERT_TRUE(shell.execute_command("truncate large.bin 0", out));
    out.str("");
    ASSERT_TRUE(shell.execute_command("stat large.bin", out));
    ASSERT_TRUE(out.str().find("Size: 0 bytes") != std::string::npos);
    ASSERT_TRUE(out.str().find("Blocks: 0") != std::string::npos);

    // 8. Remove files
    ASSERT_TRUE(shell.execute_command("rm test.txt", out));
    ASSERT_TRUE(shell.execute_command("rm large.bin", out));
    ASSERT_TRUE(shell.execute_command("rm empty.txt", out));

    out.str("");
    ASSERT_TRUE(shell.execute_command("cat test.txt", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);
}

// Scenario D: Persistence
// 1. Create directories and files
// 2. Write data into files
// 3. Exit Filler
// 4. Restart Filler using the same disk image
// 5. Verify all previously created files, directories, and contents
void test_scenario_d_persistence() {
    ScopedTestFile img("build/test_e2e_scenario_d.img");

    std::uint64_t saved_free_blocks = 0;
    std::uint64_t saved_free_inodes = 0;

    // Session 1: Create directories, files, and populate data
    {
        Shell shell1(img.path());
        std::ostringstream out;
        shell1.format_filesystem(out);

        ASSERT_TRUE(shell1.execute_command("mkdir docs", out));
        ASSERT_TRUE(shell1.execute_command("mkdir docs/specs", out));
        ASSERT_TRUE(shell1.execute_command("write docs/specs/filler.txt \"Filler Mini File System Spec v1.0\"", out));
        ASSERT_TRUE(shell1.execute_command("mkdir src", out));
        ASSERT_TRUE(shell1.execute_command("write src/main.cpp \"int main() { return 0; }\"", out));

        // Save metadata counts before exiting
        BlockDevice dev(img.path());
        Superblock sb = Superblock::read(dev);
        saved_free_blocks = sb.free_blocks;
        saved_free_inodes = sb.free_inodes;
    }

    // Session 2: Reopen without formatting and verify everything survived intact
    {
        Shell shell2(img.path());
        ASSERT_TRUE(shell2.is_formatted());
        ASSERT_EQ(shell2.current_path(), "/");

        std::ostringstream out;

        // Verify root directory contents
        out.str("");
        ASSERT_TRUE(shell2.execute_command("ls", out));
        ASSERT_TRUE(out.str().find("[DIR]  docs") != std::string::npos);
        ASSERT_TRUE(out.str().find("[DIR]  src") != std::string::npos);

        // Verify nested directory and file
        out.str("");
        ASSERT_TRUE(shell2.execute_command("cat docs/specs/filler.txt", out));
        ASSERT_EQ(out.str(), "Filler Mini File System Spec v1.0\n");

        out.str("");
        ASSERT_TRUE(shell2.execute_command("cat src/main.cpp", out));
        ASSERT_EQ(out.str(), "int main() { return 0; }\n");

        // Verify Superblock free counts preserved
        BlockDevice dev(img.path());
        Superblock sb = Superblock::read(dev);
        ASSERT_EQ(sb.free_blocks, saved_free_blocks);
        ASSERT_EQ(sb.free_inodes, saved_free_inodes);
    }
}

// Scenario E: Resource Management
// 1. Allocate multiple inodes and blocks
// 2. Delete files and directories
// 3. Verify resources are released
// 4. Test behavior when inode or block allocation fails
// 5. Check for leaked allocations and corrupted metadata
void test_scenario_e_resource_management() {
    ScopedTestFile img("build/test_e2e_scenario_e.img");
    Shell shell(img.path());
    std::ostringstream out;
    shell.format_filesystem(out);

    BlockDevice dev(img.path());
    const Superblock baseline_sb = Superblock::read(dev);
    const std::uint64_t initial_free_blocks = baseline_sb.free_blocks;
    const std::uint64_t initial_free_inodes = baseline_sb.free_inodes;

    // 1. Allocate 25 files with data
    constexpr std::size_t NUM_FILES = 25;
    for (std::size_t i = 0; i < NUM_FILES; ++i) {
        std::string fname = "file_" + std::to_string(i) + ".dat";
        std::string cmd = "write " + fname + " \"Content of file number " + std::to_string(i) + "\"";
        ASSERT_TRUE(shell.execute_command(cmd, out));
    }

    Superblock active_sb = Superblock::read(dev);
    ASSERT_EQ(active_sb.free_inodes, initial_free_inodes - NUM_FILES);
    ASSERT_EQ(active_sb.free_blocks, initial_free_blocks - NUM_FILES);

    // 2. Delete all 25 files
    for (std::size_t i = 0; i < NUM_FILES; ++i) {
        std::string fname = "file_" + std::to_string(i) + ".dat";
        ASSERT_TRUE(shell.execute_command("rm " + fname, out));
    }

    // 3. Verify resources are released completely back to baseline
    Superblock released_sb = Superblock::read(dev);
    ASSERT_EQ(released_sb.free_inodes, initial_free_inodes);
    ASSERT_EQ(released_sb.free_blocks, initial_free_blocks);

    // Verify block bitmap count matches
    BlockBitmap bb(dev, released_sb);
    ASSERT_EQ(bb.count_free_blocks(), initial_free_blocks);

    // 4. Test inode exhaustion: allocate remaining inodes up to capacity
    // Baseline free inodes = 254. Allocate all of them.
    std::vector<std::uint64_t> allocated_inodes;
    allocated_inodes.reserve(initial_free_inodes);
    for (std::uint64_t i = 0; i < initial_free_inodes; ++i) {
        std::uint64_t ino = shell.inode_manager()->allocate_inode();
        allocated_inodes.push_back(ino);
    }

    Superblock exhausted_sb = Superblock::read(dev);
    ASSERT_EQ(exhausted_sb.free_inodes, 0ULL);

    // Trying to allocate one more must throw NoFreeInodesError
    bool threw_exhaustion = false;
    try {
        (void)shell.inode_manager()->allocate_inode();
    } catch (const NoFreeInodesError&) {
        threw_exhaustion = true;
    }
    ASSERT_TRUE(threw_exhaustion);

    // Free one inode and verify reallocation succeeds
    const std::uint64_t freed_ino = allocated_inodes.back();
    allocated_inodes.pop_back();
    shell.inode_manager()->free_inode(freed_ino);

    ASSERT_EQ(Superblock::read(dev).free_inodes, 1ULL);
    const std::uint64_t reallocated = shell.inode_manager()->allocate_inode();
    ASSERT_EQ(reallocated, freed_ino);
    allocated_inodes.push_back(reallocated);

    // Clean up all allocated inodes
    for (std::uint64_t ino : allocated_inodes) {
        shell.inode_manager()->free_inode(ino);
    }
    ASSERT_EQ(Superblock::read(dev).free_inodes, initial_free_inodes);
}

// Scenario F: Invalid Operations
// Test:
// - Invalid commands
// - Missing files
// - Invalid paths
// - Duplicate names
// - Invalid filenames
// - Reading beyond EOF
// - Removing non-empty directories
// - Invalid disk images
// - Insufficient storage
void test_scenario_f_invalid_operations() {
    ScopedTestFile img("build/test_e2e_scenario_f.img");
    Shell shell(img.path());
    std::ostringstream out;
    shell.format_filesystem(out);

    // 1. Invalid command
    out.str("");
    ASSERT_TRUE(shell.execute_command("invalid_unknown_cmd arg1 arg2", out));
    ASSERT_TRUE(out.str().find("Unknown command") != std::string::npos);

    // 2. Missing files
    out.str("");
    ASSERT_TRUE(shell.execute_command("cat missing_file.txt", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);

    out.str("");
    ASSERT_TRUE(shell.execute_command("stat missing_file.txt", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);

    out.str("");
    ASSERT_TRUE(shell.execute_command("rm missing_file.txt", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);

    // 3. Invalid paths
    out.str("");
    ASSERT_TRUE(shell.execute_command("cd /invalid/nonexistent/path", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);
    ASSERT_EQ(shell.current_path(), "/");

    // 4. Duplicate names
    ASSERT_TRUE(shell.execute_command("mkdir duplicate_item", out));
    out.str("");
    ASSERT_TRUE(shell.execute_command("mkdir duplicate_item", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);

    // 5. Invalid filenames (slash, null, empty)
    out.str("");
    ASSERT_TRUE(shell.execute_command("mkdir \"invalid/name\"", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);

    // 6. Reading beyond EOF
    ASSERT_TRUE(shell.execute_command("write short.txt \"abc\"", out));
    std::uint64_t file_ino = shell.resolve_path("short.txt");
    alignas(8) std::uint8_t buf[64];
    std::size_t bytes_read = shell.file_manager()->read(file_ino, 100, buf, sizeof(buf));
    ASSERT_EQ(bytes_read, 0ULL);

    // 7. Removing directory with rm (rejected)
    out.str("");
    ASSERT_TRUE(shell.execute_command("rm duplicate_item", out));
    ASSERT_TRUE(out.str().find("use rmdir") != std::string::npos);

    // 8. Removing file with rmdir (rejected)
    out.str("");
    ASSERT_TRUE(shell.execute_command("rmdir short.txt", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);

    // 9. cd into regular file (rejected)
    out.str("");
    ASSERT_TRUE(shell.execute_command("cd short.txt", out));
    ASSERT_TRUE(out.str().find("Not a directory") != std::string::npos);
    ASSERT_EQ(shell.current_path(), "/");
}

// Data Integrity & Consistency Checks (Direct Raw Disk Inspection)
void test_data_integrity_and_consistency() {
    ScopedTestFile img("build/test_e2e_data_integrity.img");

    {
        Shell shell(img.path());
        std::ostringstream out;
        shell.format_filesystem(out);

        // Build a multi-level tree with multiple files and directories
        ASSERT_TRUE(shell.execute_command("mkdir dir_a", out));
        ASSERT_TRUE(shell.execute_command("mkdir dir_b", out));
        ASSERT_TRUE(shell.execute_command("mkdir dir_a/nested", out));

        ASSERT_TRUE(shell.execute_command("write file1.txt \"Data in file 1\"", out));
        ASSERT_TRUE(shell.execute_command("write dir_a/file2.txt \"Data in file 2 under dir_a\"", out));
        ASSERT_TRUE(shell.execute_command("write dir_a/nested/file3.txt \"Data in nested file 3\"", out));
        ASSERT_TRUE(shell.execute_command("write dir_b/file4.txt \"Data in file 4 under dir_b\"", out));
    }

    // Inspect the disk image directly through BlockDevice
    BlockDevice dev(img.path());
    Superblock sb = Superblock::read(dev);
    sb.validate();

    // 1. Inode bitmap verification
    BlockDevice dev_inodes(img.path());
    InodeManager im(dev_inodes, sb);

    std::uint64_t allocated_inode_count = 0;
    std::set<std::uint64_t> allocated_inodes;
    for (std::uint64_t ino = 0; ino < sb.total_inodes; ++ino) {
        if (im.is_inode_allocated(ino)) {
            ++allocated_inode_count;
            allocated_inodes.insert(ino);
        }
    }
    // Inode 0 (reserved) + root inode 1 + 3 dirs + 4 files = 9 allocated inodes
    ASSERT_EQ(allocated_inode_count, 9ULL);
    ASSERT_EQ(sb.free_inodes, sb.total_inodes - allocated_inode_count);

    // 2. Block bitmap verification & block collision check
    BlockDevice dev_blocks(img.path());
    BlockBitmap bb(dev_blocks, sb);

    std::uint64_t allocated_block_count = 0;
    for (std::uint64_t blk = 0; blk < sb.total_blocks; ++blk) {
        if (bb.is_allocated(blk)) {
            ++allocated_block_count;
        }
    }
    // 19 metadata blocks + 4 directory blocks (root, dir_a, dir_b, nested) + 4 file blocks = 27 allocated blocks
    ASSERT_EQ(allocated_block_count, 19ULL + 4ULL + 4ULL);
    ASSERT_EQ(sb.free_blocks, sb.data_region_blocks - 8ULL);

    // 3. No two files/directories share any data block
    std::set<std::uint64_t> seen_blocks;
    for (std::uint64_t ino : allocated_inodes) {
        if (ino == 0) continue; // Inode 0 has no blocks
        Inode inode = im.get_inode(ino);
        for (std::size_t b = 0; b < inode.blocks_count && b < Inode::DIRECT_BLOCKS_COUNT; ++b) {
            std::uint64_t phys_block = inode.direct_blocks[b];
            if (phys_block != 0) {
                // Must be in data region
                ASSERT_TRUE(phys_block >= sb.data_region_start);
                ASSERT_TRUE(phys_block < sb.total_blocks);
                // Must be marked allocated in bitmap
                ASSERT_TRUE(bb.is_allocated(phys_block));
                // Must NOT be shared with any other inode!
                ASSERT_TRUE(seen_blocks.find(phys_block) == seen_blocks.end());
                seen_blocks.insert(phys_block);
            }
        }
    }

    // 4. Verify all directory entries point to valid, allocated inodes
    DirectoryManager dm(dev, im, bb);
    std::vector<std::uint64_t> dirs_to_check = {1}; // Start with root
    while (!dirs_to_check.empty()) {
        std::uint64_t cur_dir = dirs_to_check.back();
        dirs_to_check.pop_back();

        auto entries = dm.list(cur_dir);
        for (const auto& entry : entries) {
            ASSERT_TRUE(allocated_inodes.find(entry.inode_id) != allocated_inodes.end());
            if (entry.name != "." && entry.name != "..") {
                Inode entry_inode = im.get_inode(entry.inode_id);
                if (entry_inode.is_directory()) {
                    dirs_to_check.push_back(entry.inode_id);
                }
            }
        }
    }
}

// Scenario G: Dynamic Capacity (256 MiB, 1 GiB, 4 GiB Sparse Images)
void test_scenario_g_dynamic_capacity_sparse_images() {
    // 1. 256 MiB image
    {
        const std::string img_path = "build/test_dynamic_256m.img";
        std::remove(img_path.c_str());
        int fd = ::open(img_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        ASSERT_TRUE(fd >= 0);
        ASSERT_EQ(::ftruncate(fd, static_cast<off_t>(256ULL * 1024ULL * 1024ULL)), 0);
        ::close(fd);

        {
            Shell shell(img_path);
            ASSERT_FALSE(shell.is_formatted());
            ASSERT_FALSE(shell.is_block_device());
            std::ostringstream out;
            shell.format_filesystem(out);
            ASSERT_TRUE(shell.is_formatted());

            BlockDevice dev(img_path);
            Superblock sb = Superblock::read(dev);
            ASSERT_EQ(sb.total_blocks, 65536ULL);
            ASSERT_EQ(sb.total_inodes, 1024ULL);
            ASSERT_EQ(sb.block_bitmap_blocks, 2ULL);
            ASSERT_EQ(sb.inode_bitmap_blocks, 1ULL);
            ASSERT_EQ(sb.inode_table_blocks, 64ULL);
            ASSERT_EQ(sb.data_region_start, 68ULL);
            ASSERT_EQ(sb.data_region_blocks, 65536ULL - 68ULL);

            ASSERT_TRUE(shell.execute_command("mkdir data", out));
            ASSERT_TRUE(shell.execute_command("cd data", out));
            ASSERT_TRUE(shell.execute_command("touch notes.txt", out));
            ASSERT_TRUE(shell.execute_command("write notes.txt \"Testing 256 MiB image\"", out));
            out.str("");
            ASSERT_TRUE(shell.execute_command("cat notes.txt", out));
            ASSERT_TRUE(out.str().find("Testing 256 MiB image") != std::string::npos);
        }

        std::remove(img_path.c_str());
    }

    // 2. 1 GiB image
    {
        const std::string img_path = "build/test_dynamic_1g.img";
        std::remove(img_path.c_str());
        int fd = ::open(img_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        ASSERT_TRUE(fd >= 0);
        ASSERT_EQ(::ftruncate(fd, static_cast<off_t>(1024ULL * 1024ULL * 1024ULL)), 0);
        ::close(fd);

        {
            Shell shell(img_path);
            std::ostringstream out;
            shell.format_filesystem(out);
            ASSERT_TRUE(shell.is_formatted());

            BlockDevice dev(img_path);
            Superblock sb = Superblock::read(dev);
            ASSERT_EQ(sb.total_blocks, 262144ULL);
            ASSERT_EQ(sb.total_inodes, 4096ULL);
            ASSERT_EQ(sb.block_bitmap_blocks, 8ULL);
            ASSERT_EQ(sb.inode_bitmap_blocks, 1ULL);
            ASSERT_EQ(sb.inode_table_blocks, 256ULL);
            ASSERT_EQ(sb.data_region_start, 266ULL);
            ASSERT_EQ(sb.data_region_blocks, 262144ULL - 266ULL);

            ASSERT_TRUE(shell.execute_command("mkdir project", out));
            ASSERT_TRUE(shell.execute_command("touch project/readme.md", out));
            ASSERT_TRUE(shell.execute_command("write project/readme.md \"Filler 1 GiB filesystem\"", out));
        }

        // Reopen in a second session and verify persistence
        {
            Shell shell(img_path);
            ASSERT_TRUE(shell.is_formatted());
            std::ostringstream out;
            ASSERT_TRUE(shell.execute_command("cat /project/readme.md", out));
            ASSERT_TRUE(out.str().find("Filler 1 GiB filesystem") != std::string::npos);
        }

        std::remove(img_path.c_str());
    }

    // 3. 4 GiB image
    {
        const std::string img_path = "build/test_dynamic_4g.img";
        std::remove(img_path.c_str());
        int fd = ::open(img_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        ASSERT_TRUE(fd >= 0);
        ASSERT_EQ(::ftruncate(fd, static_cast<off_t>(4ULL * 1024ULL * 1024ULL * 1024ULL)), 0);
        ::close(fd);

        {
            Shell shell(img_path);
            std::ostringstream out;
            shell.format_filesystem(out);
            ASSERT_TRUE(shell.is_formatted());

            BlockDevice dev(img_path);
            Superblock sb = Superblock::read(dev);
            ASSERT_EQ(sb.total_blocks, 1048576ULL);
            ASSERT_EQ(sb.total_inodes, 16384ULL);
            ASSERT_EQ(sb.block_bitmap_blocks, 32ULL);
            ASSERT_EQ(sb.inode_bitmap_blocks, 1ULL);
            ASSERT_EQ(sb.inode_table_blocks, 1024ULL);
            ASSERT_EQ(sb.data_region_start, 1058ULL);
            ASSERT_EQ(sb.data_region_blocks, 1048576ULL - 1058ULL);

            ASSERT_TRUE(shell.execute_command("mkdir large_test", out));
            ASSERT_TRUE(shell.execute_command("touch large_test/huge.txt", out));
            ASSERT_TRUE(shell.execute_command("write large_test/huge.txt \"4 GiB block device scale verification\"", out));
            out.str("");
            ASSERT_TRUE(shell.execute_command("cat large_test/huge.txt", out));
            ASSERT_TRUE(out.str().find("4 GiB block device scale verification") != std::string::npos);
        }

        // Second session verification
        {
            Shell shell(img_path);
            ASSERT_TRUE(shell.is_formatted());
            std::ostringstream out;
            ASSERT_TRUE(shell.execute_command("cat /large_test/huge.txt", out));
            ASSERT_TRUE(out.str().find("4 GiB block device scale verification") != std::string::npos);
        }

        std::remove(img_path.c_str());
    }
}

// Scenario H: Block Device Safety & Properties
void test_scenario_h_block_device_safety() {
    const std::string img_path = "build/test_safety_reg.img";
    std::remove(img_path.c_str());
    int fd = ::open(img_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(::ftruncate(fd, 64 * 1024 * 1024), 0);
    ::close(fd);

    Shell shell(img_path);
    ASSERT_FALSE(shell.is_block_device());
    ASSERT_TRUE(shell.device() != nullptr);
    ASSERT_FALSE(shell.device()->is_block_device());
    ASSERT_EQ(shell.device()->path(), img_path);

    std::remove(img_path.c_str());
}

// ============================================================================
// Integration Test Runner
// ============================================================================

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

} // namespace

int main() {
    const std::vector<TestCase> test_cases = {
        {"Scenario A: Basic Filesystem", test_scenario_a_basic_filesystem},
        {"Scenario B: Directory Operations", test_scenario_b_directory_operations},
        {"Scenario C: File Operations", test_scenario_c_file_operations},
        {"Scenario D: Persistence", test_scenario_d_persistence},
        {"Scenario E: Resource Management", test_scenario_e_resource_management},
        {"Scenario F: Invalid Operations", test_scenario_f_invalid_operations},
        {"Data Integrity & Consistency", test_data_integrity_and_consistency},
        {"Scenario G: Dynamic Capacity (256M, 1G, 4G)", test_scenario_g_dynamic_capacity_sparse_images},
        {"Scenario H: Block Device Safety & Properties", test_scenario_h_block_device_safety},
    };

    std::cout << "======================================================================\n";
    std::cout << "         Filler Mini File System: End-to-End Integration Suite        \n";
    std::cout << "======================================================================\n";

    std::size_t passed = 0;
    std::size_t failed = 0;

    for (const auto& tc : test_cases) {
        try {
            tc.fn();
            std::cout << "  [PASS] " << tc.name << "\n";
            ++passed;
        } catch (const TestFailure& tf) {
            std::cout << "  [FAIL] " << tc.name << "\n";
            std::cout << "         " << tf.what() << "\n";
            ++failed;
        } catch (const std::exception& ex) {
            std::cout << "  [FAIL] " << tc.name << " (unexpected exception: " << ex.what() << ")\n";
            ++failed;
        } catch (...) {
            std::cout << "  [FAIL] " << tc.name << " (unknown exception)\n";
            ++failed;
        }
    }

    std::cout << "======================================================================\n";
    std::cout << "Test Summary: " << passed << " / " << test_cases.size() << " passed ("
              << failed << " failed)\n";
    std::cout << "======================================================================\n";

    return (failed == 0) ? 0 : 1;
}

