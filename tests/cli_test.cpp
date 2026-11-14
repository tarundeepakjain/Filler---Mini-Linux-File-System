#include "cli/Shell.hpp"
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

using filler::cli::Shell;
using filler::cli::FilesystemState;
using filler::fs::FileType;
using filler::fs::Superblock;

inline std::ostream& operator<<(std::ostream& os, FilesystemState state) {
    switch (state) {
        case FilesystemState::VALID: return os << "VALID";
        case FilesystemState::UNFORMATTED: return os << "UNFORMATTED";
        case FilesystemState::INVALID_IMAGE: return os << "INVALID_IMAGE";
        case FilesystemState::CORRUPTED: return os << "CORRUPTED";
        case FilesystemState::IO_ERROR: return os << "IO_ERROR";
    }
    return os << "UNKNOWN";
}

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
// RAII Test File Cleanup Helper
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
// Test Cases
// ============================================================================

void test_unformatted_and_format() {
    ScopedTestFile test_img("build/test_cli_unformatted.img");
    Shell shell(test_img.path());

    ASSERT_FALSE(shell.is_formatted());

    std::ostringstream out;
    ASSERT_TRUE(shell.execute_command("pwd", out));
    ASSERT_TRUE(out.str().find("Error: Filesystem is not formatted") != std::string::npos);

    out.str("");
    ASSERT_TRUE(shell.execute_command("ls", out));
    ASSERT_TRUE(out.str().find("Error: Filesystem is not formatted") != std::string::npos);

    out.str("");
    ASSERT_TRUE(shell.execute_command("mkdir test", out));
    ASSERT_TRUE(out.str().find("Error: Filesystem is not formatted") != std::string::npos);

    // Format disk
    out.str("");
    ASSERT_TRUE(shell.execute_command("format", out));
    ASSERT_TRUE(out.str().find("successfully formatted") != std::string::npos);
    ASSERT_TRUE(shell.is_formatted());
    ASSERT_EQ(shell.current_path(), "/");
    ASSERT_EQ(shell.current_dir_inode(), Superblock::DEFAULT_ROOT_INODE);

    // Now pwd works
    out.str("");
    ASSERT_TRUE(shell.execute_command("pwd", out));
    ASSERT_EQ(out.str(), "/\n");
}

void test_help_and_exit() {
    ScopedTestFile test_img("build/test_cli_help_exit.img");
    Shell shell(test_img.path());

    std::ostringstream out;
    ASSERT_TRUE(shell.execute_command("help", out));
    ASSERT_TRUE(out.str().find("Available commands:") != std::string::npos);
    ASSERT_TRUE(out.str().find("format") != std::string::npos);
    ASSERT_TRUE(out.str().find("mkdir") != std::string::npos);
    ASSERT_TRUE(out.str().find("touch") != std::string::npos);
    ASSERT_TRUE(out.str().find("exit") != std::string::npos);

    // Exit and quit return false
    out.str("");
    ASSERT_FALSE(shell.execute_command("exit", out));
    ASSERT_FALSE(shell.execute_command("quit", out));

    // Unknown command
    shell.format_filesystem(out);
    out.str("");
    ASSERT_TRUE(shell.execute_command("unknowncmd", out));
    ASSERT_TRUE(out.str().find("Unknown command") != std::string::npos);
}

void test_pwd_cd_navigation() {
    ScopedTestFile test_img("build/test_cli_cd.img");
    Shell shell(test_img.path());
    std::ostringstream out;
    shell.format_filesystem(out);

    ASSERT_EQ(shell.current_path(), "/");

    // Create directories
    ASSERT_TRUE(shell.execute_command("mkdir docs", out));
    ASSERT_TRUE(shell.execute_command("mkdir docs/work", out));

    // cd into child
    out.str("");
    ASSERT_TRUE(shell.execute_command("cd docs", out));
    ASSERT_EQ(shell.current_path(), "/docs");

    out.str("");
    ASSERT_TRUE(shell.execute_command("pwd", out));
    ASSERT_EQ(out.str(), "/docs\n");

    // cd into nested child
    ASSERT_TRUE(shell.execute_command("cd work", out));
    ASSERT_EQ(shell.current_path(), "/docs/work");

    // cd ..
    ASSERT_TRUE(shell.execute_command("cd ..", out));
    ASSERT_EQ(shell.current_path(), "/docs");

    // cd /
    ASSERT_TRUE(shell.execute_command("cd /", out));
    ASSERT_EQ(shell.current_path(), "/");

    // cd with absolute path
    ASSERT_TRUE(shell.execute_command("cd /docs/work", out));
    ASSERT_EQ(shell.current_path(), "/docs/work");

    // cd with multiple dots
    ASSERT_TRUE(shell.execute_command("cd ../..", out));
    ASSERT_EQ(shell.current_path(), "/");

    // cd into nonexistent directory
    out.str("");
    ASSERT_TRUE(shell.execute_command("cd nonexistent", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);
    ASSERT_EQ(shell.current_path(), "/");
}

void test_ls_and_mkdir() {
    ScopedTestFile test_img("build/test_cli_ls_mkdir.img");
    Shell shell(test_img.path());
    std::ostringstream out;
    shell.format_filesystem(out);

    ASSERT_TRUE(shell.execute_command("mkdir a", out));
    ASSERT_TRUE(shell.execute_command("mkdir b", out));

    out.str("");
    ASSERT_TRUE(shell.execute_command("ls", out));
    const std::string ls_root = out.str();
    ASSERT_TRUE(ls_root.find("[DIR]  .") != std::string::npos);
    ASSERT_TRUE(ls_root.find("[DIR]  ..") != std::string::npos);
    ASSERT_TRUE(ls_root.find("[DIR]  a") != std::string::npos);
    ASSERT_TRUE(ls_root.find("[DIR]  b") != std::string::npos);

    // List specific directory
    ASSERT_TRUE(shell.execute_command("touch a/test.txt", out));
    out.str("");
    ASSERT_TRUE(shell.execute_command("ls a", out));
    const std::string ls_a = out.str();
    ASSERT_TRUE(ls_a.find("[FILE] test.txt") != std::string::npos);
}

void test_file_operations_write_append_cat() {
    ScopedTestFile test_img("build/test_cli_file_ops.img");
    Shell shell(test_img.path());
    std::ostringstream out;
    shell.format_filesystem(out);

    // Touch
    ASSERT_TRUE(shell.execute_command("touch note.txt", out));
    out.str("");
    ASSERT_TRUE(shell.execute_command("cat note.txt", out));
    ASSERT_EQ(out.str(), "");

    // Write quoted text
    out.str("");
    ASSERT_TRUE(shell.execute_command("write note.txt \"First line.\"", out));
    ASSERT_TRUE(shell.execute_command("cat note.txt", out));
    ASSERT_EQ(out.str(), "First line.\n");

    // Append text with escape newline
    out.str("");
    ASSERT_TRUE(shell.execute_command("append note.txt \"\\nSecond line.\"", out));
    ASSERT_TRUE(shell.execute_command("cat note.txt", out));
    ASSERT_EQ(out.str(), "First line.\nSecond line.\n");

    // Overwrite file
    out.str("");
    ASSERT_TRUE(shell.execute_command("write note.txt \"Overwritten.\"", out));
    ASSERT_TRUE(shell.execute_command("cat note.txt", out));
    ASSERT_EQ(out.str(), "Overwritten.\n");

    // Writing to a new file directly without touch first
    out.str("");
    ASSERT_TRUE(shell.execute_command("write auto.txt hello", out));
    ASSERT_TRUE(shell.execute_command("cat auto.txt", out));
    ASSERT_EQ(out.str(), "hello\n");
}

void test_rm_and_rmdir() {
    ScopedTestFile test_img("build/test_cli_rm_rmdir.img");
    Shell shell(test_img.path());
    std::ostringstream out;
    shell.format_filesystem(out);

    ASSERT_TRUE(shell.execute_command("mkdir folder", out));
    ASSERT_TRUE(shell.execute_command("touch folder/data.bin", out));

    // rm directory fails
    out.str("");
    ASSERT_TRUE(shell.execute_command("rm folder", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);

    // rmdir non-empty directory fails
    out.str("");
    ASSERT_TRUE(shell.execute_command("rmdir folder", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);

    // rm file succeeds
    out.str("");
    ASSERT_TRUE(shell.execute_command("rm folder/data.bin", out));
    ASSERT_TRUE(out.str().find("Error") == std::string::npos);

    // cat removed file fails
    out.str("");
    ASSERT_TRUE(shell.execute_command("cat folder/data.bin", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);

    // rmdir now-empty directory succeeds
    out.str("");
    ASSERT_TRUE(shell.execute_command("rmdir folder", out));
    ASSERT_TRUE(out.str().find("Error") == std::string::npos);

    // rmdir root or special directories rejected
    out.str("");
    ASSERT_TRUE(shell.execute_command("rmdir /", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);

    out.str("");
    ASSERT_TRUE(shell.execute_command("rmdir .", out));
    ASSERT_TRUE(out.str().find("Error") != std::string::npos);
}

void test_stat_command() {
    ScopedTestFile test_img("build/test_cli_stat.img");
    Shell shell(test_img.path());
    std::ostringstream out;
    shell.format_filesystem(out);

    ASSERT_TRUE(shell.execute_command("mkdir mydir", out));
    ASSERT_TRUE(shell.execute_command("write mydir/file.txt \"Hello\"", out));

    // Stat directory
    out.str("");
    ASSERT_TRUE(shell.execute_command("stat mydir", out));
    ASSERT_TRUE(out.str().find("Type: Directory") != std::string::npos);
    ASSERT_TRUE(out.str().find("Inode:") != std::string::npos);

    // Stat file
    out.str("");
    ASSERT_TRUE(shell.execute_command("stat mydir/file.txt", out));
    ASSERT_TRUE(out.str().find("Type: Regular File") != std::string::npos);
    ASSERT_TRUE(out.str().find("Size: 5 bytes") != std::string::npos);
    ASSERT_TRUE(out.str().find("Direct blocks:") != std::string::npos);
}

void test_persistence_across_sessions() {
    ScopedTestFile test_img("build/test_cli_persistence.img");

    // Session 1: format and write data
    {
        Shell shell1(test_img.path());
        std::ostringstream out;
        shell1.format_filesystem(out);
        ASSERT_TRUE(shell1.execute_command("mkdir session1", out));
        ASSERT_TRUE(shell1.execute_command("write session1/persisted.txt \"Permanent record\"", out));
    }

    // Session 2: open existing image without formatting
    {
        Shell shell2(test_img.path());
        ASSERT_TRUE(shell2.is_formatted());
        ASSERT_EQ(shell2.current_path(), "/");

        std::ostringstream out;
        ASSERT_TRUE(shell2.execute_command("ls session1", out));
        ASSERT_TRUE(out.str().find("persisted.txt") != std::string::npos);

        out.str("");
        ASSERT_TRUE(shell2.execute_command("cat session1/persisted.txt", out));
        ASSERT_EQ(out.str(), "Permanent record\n");
    }
}

void test_interactive_repl_run() {
    ScopedTestFile test_img("build/test_cli_repl.img");
    Shell shell(test_img.path());

    std::istringstream in(
        "format\n"
        "pwd\n"
        "mkdir myfolder\n"
        "cd myfolder\n"
        "pwd\n"
        "write message.txt \"Hello from REPL\"\n"
        "cat message.txt\n"
        "exit\n"
    );
    std::ostringstream out;
    shell.run(in, out);

    const std::string repl_output = out.str();
    ASSERT_TRUE(repl_output.find("filler:/> ") != std::string::npos);
    ASSERT_TRUE(repl_output.find("filler:/myfolder> ") != std::string::npos);
    ASSERT_TRUE(repl_output.find("Filesystem successfully formatted.") != std::string::npos);
    ASSERT_TRUE(repl_output.find("Hello from REPL") != std::string::npos);
}

void test_format_undersized_image() {
    ScopedTestFile test_img("build/test_cli_undersized.img");
    // Create an undersized image file of 65,536 bytes (16 blocks)
    {
        std::ofstream ofs(test_img.path(), std::ios::binary | std::ios::trunc);
        std::vector<char> buf(65536, 0);
        ofs.write(buf.data(), buf.size());
    }

    Shell shell(test_img.path());
    std::ostringstream out;
    ASSERT_TRUE(shell.execute_command("format", out));
    ASSERT_TRUE(out.str().find("successfully formatted") != std::string::npos);
    ASSERT_TRUE(shell.is_formatted());
    ASSERT_EQ(shell.current_path(), "/");

    // Can create directories and files
    ASSERT_TRUE(shell.execute_command("mkdir check", out));
    out.str("");
    ASSERT_TRUE(shell.execute_command("ls", out));
    ASSERT_TRUE(out.str().find("[DIR]  check") != std::string::npos);
}

void test_filesystem_state_classification() {
    // 1. Opening a valid formatted image
    {
        ScopedTestFile valid_img("build/test_state_valid.img");
        {
            Shell shell1(valid_img.path());
            std::ostringstream out;
            shell1.format_filesystem(out);
            ASSERT_TRUE(shell1.is_formatted());
            ASSERT_EQ(shell1.filesystem_state(), FilesystemState::VALID);
        }
        // Reopen existing formatted image
        Shell shell2(valid_img.path());
        ASSERT_TRUE(shell2.is_formatted());
        ASSERT_EQ(shell2.filesystem_state(), FilesystemState::VALID);
        ASSERT_TRUE(shell2.status_message().find("valid") != std::string::npos);
        std::ostringstream out;
        ASSERT_TRUE(shell2.execute_command("pwd", out));
        ASSERT_EQ(out.str(), "/\n");
    }

    // 2. Opening a newly created unformatted image
    {
        ScopedTestFile unformatted_img("build/test_state_unformatted.img");
        Shell shell(unformatted_img.path());
        ASSERT_FALSE(shell.is_formatted());
        ASSERT_EQ(shell.filesystem_state(), FilesystemState::UNFORMATTED);
        ASSERT_TRUE(shell.status_message().find("unformatted") != std::string::npos);
        std::ostringstream out;
        ASSERT_TRUE(shell.execute_command("ls", out));
        ASSERT_TRUE(out.str().find("Error: Filesystem is not formatted. Run 'format' first.") != std::string::npos);
    }

    // 3. Opening an image with an invalid magic number
    {
        ScopedTestFile bad_magic_img("build/test_state_bad_magic.img");
        {
            Shell shell(bad_magic_img.path());
            std::ostringstream out;
            shell.format_filesystem(out);
        }
        // Corrupt magic number in Block 0
        {
            std::fstream fs(bad_magic_img.path(), std::ios::in | std::ios::out | std::ios::binary);
            uint32_t bad_magic = 0xDEADBEEFU;
            fs.seekp(0);
            fs.write(reinterpret_cast<const char*>(&bad_magic), sizeof(bad_magic));
        }
        Shell shell(bad_magic_img.path());
        ASSERT_FALSE(shell.is_formatted());
        ASSERT_EQ(shell.filesystem_state(), FilesystemState::INVALID_IMAGE);
        ASSERT_TRUE(shell.status_message().find("unrecognized magic number") != std::string::npos);
        std::ostringstream out;
        ASSERT_TRUE(shell.execute_command("ls", out));
        ASSERT_TRUE(out.str().find("unrecognized magic number") != std::string::npos);
    }

    // 4. Opening an image with an unsupported version
    {
        ScopedTestFile bad_ver_img("build/test_state_bad_ver.img");
        {
            Shell shell(bad_ver_img.path());
            std::ostringstream out;
            shell.format_filesystem(out);
        }
        // Corrupt version number in Block 0 (offset 4)
        {
            std::fstream fs(bad_ver_img.path(), std::ios::in | std::ios::out | std::ios::binary);
            uint32_t bad_version = 99U;
            fs.seekp(4);
            fs.write(reinterpret_cast<const char*>(&bad_version), sizeof(bad_version));
        }
        Shell shell(bad_ver_img.path());
        ASSERT_FALSE(shell.is_formatted());
        ASSERT_EQ(shell.filesystem_state(), FilesystemState::INVALID_IMAGE);
        ASSERT_TRUE(shell.status_message().find("unsupported version") != std::string::npos);
        std::ostringstream out;
        ASSERT_TRUE(shell.execute_command("pwd", out));
        ASSERT_TRUE(out.str().find("unsupported version") != std::string::npos);
    }

    // 5. Opening a truncated or malformed image (non-multiple of block size)
    {
        ScopedTestFile malformed_img("build/test_state_malformed.img");
        {
            std::ofstream ofs(malformed_img.path(), std::ios::binary | std::ios::trunc);
            std::vector<char> buf(500, 'M');
            ofs.write(buf.data(), buf.size());
        }
        Shell shell(malformed_img.path());
        ASSERT_FALSE(shell.is_formatted());
        ASSERT_EQ(shell.filesystem_state(), FilesystemState::IO_ERROR);
        ASSERT_TRUE(shell.status_message().find("Disk I/O") != std::string::npos);
        std::ostringstream out;
        ASSERT_TRUE(shell.execute_command("mkdir test", out));
        ASSERT_TRUE(out.str().find("Disk I/O") != std::string::npos);
    }

    // 6. Simulating a disk I/O error where practical (e.g. non-existent directory path)
    {
        Shell shell("/non_existent_dir_99999/test.img");
        ASSERT_FALSE(shell.is_formatted());
        ASSERT_EQ(shell.filesystem_state(), FilesystemState::IO_ERROR);
        ASSERT_TRUE(shell.status_message().find("Disk I/O") != std::string::npos);
        std::ostringstream out;
        ASSERT_TRUE(shell.execute_command("ls", out));
        ASSERT_TRUE(out.str().find("Disk I/O") != std::string::npos);
    }

    // 7. Opening an image with corrupted filesystem metadata (valid magic & version, but invalid fields)
    {
        ScopedTestFile corrupted_img("build/test_state_corrupted.img");
        {
            Shell shell(corrupted_img.path());
            std::ostringstream out;
            shell.format_filesystem(out);
        }
        // Corrupt data region start in Block 0 (offset 104) to an out-of-range value
        {
            std::fstream fs(corrupted_img.path(), std::ios::in | std::ios::out | std::ios::binary);
            uint64_t invalid_data_start = 99999999ULL;
            fs.seekp(filler::fs::offsets::DATA_REGION_START);
            fs.write(reinterpret_cast<const char*>(&invalid_data_start), sizeof(invalid_data_start));
        }
        Shell shell(corrupted_img.path());
        ASSERT_FALSE(shell.is_formatted());
        ASSERT_EQ(shell.filesystem_state(), FilesystemState::CORRUPTED);
        ASSERT_TRUE(shell.status_message().find("Corrupted filesystem metadata") != std::string::npos);
        std::ostringstream out;
        ASSERT_TRUE(shell.execute_command("ls", out));
        ASSERT_TRUE(out.str().find("Corrupted filesystem metadata") != std::string::npos);
    }

    // 8. Opening an image with erased magic number (0) but existing metadata
    {
        ScopedTestFile erased_magic_img("build/test_state_erased_magic.img");
        {
            Shell shell(erased_magic_img.path());
            std::ostringstream out;
            shell.format_filesystem(out);
        }
        // Zero out only magic number (4 bytes at offset 0)
        {
            std::fstream fs(erased_magic_img.path(), std::ios::in | std::ios::out | std::ios::binary);
            uint32_t zero_magic = 0;
            fs.seekp(0);
            fs.write(reinterpret_cast<const char*>(&zero_magic), sizeof(zero_magic));
        }
        Shell shell(erased_magic_img.path());
        ASSERT_FALSE(shell.is_formatted());
        ASSERT_EQ(shell.filesystem_state(), FilesystemState::CORRUPTED);
        ASSERT_TRUE(shell.status_message().find("magic number is zero") != std::string::npos);
        std::ostringstream out;
        ASSERT_TRUE(shell.execute_command("ls", out));
        ASSERT_TRUE(out.str().find("Corrupted filesystem") != std::string::npos);
    }

    // 9. Opening an image with completely wiped block 0, but bitmap data present
    {
        ScopedTestFile wiped_b0_img("build/test_state_wiped_b0.img");
        {
            Shell shell(wiped_b0_img.path());
            std::ostringstream out;
            shell.format_filesystem(out);
        }
        // Zero out entire block 0
        {
            std::fstream fs(wiped_b0_img.path(), std::ios::in | std::ios::out | std::ios::binary);
            std::vector<char> zero_block(4096, 0);
            fs.seekp(0);
            fs.write(zero_block.data(), zero_block.size());
        }
        Shell shell(wiped_b0_img.path());
        ASSERT_FALSE(shell.is_formatted());
        ASSERT_EQ(shell.filesystem_state(), FilesystemState::CORRUPTED);
        ASSERT_TRUE(shell.status_message().find("Corrupted filesystem") != std::string::npos);
    }
}

// ============================================================================
// Test Suite Runner
// ============================================================================

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

} // namespace

int main() {
    const std::vector<TestCase> test_cases = {
        {"test_unformatted_and_format", test_unformatted_and_format},
        {"test_help_and_exit", test_help_and_exit},
        {"test_pwd_cd_navigation", test_pwd_cd_navigation},
        {"test_ls_and_mkdir", test_ls_and_mkdir},
        {"test_file_operations_write_append_cat", test_file_operations_write_append_cat},
        {"test_rm_and_rmdir", test_rm_and_rmdir},
        {"test_stat_command", test_stat_command},
        {"test_persistence_across_sessions", test_persistence_across_sessions},
        {"test_interactive_repl_run", test_interactive_repl_run},
        {"test_format_undersized_image", test_format_undersized_image},
        {"test_filesystem_state_classification", test_filesystem_state_classification},
    };

    std::cout << "==================================================\n";
    std::cout << "Running Filler Shell CLI Test Suite (" << test_cases.size() << " tests)\n";
    std::cout << "==================================================\n";

    std::size_t passed = 0;
    std::size_t failed = 0;

    for (const auto& tc : test_cases) {
        try {
            tc.fn();
            std::cout << "[PASS] " << tc.name << "\n";
            ++passed;
        } catch (const TestFailure& tf) {
            std::cout << "[FAIL] " << tc.name << "\n";
            std::cout << "       " << tf.what() << "\n";
            ++failed;
        } catch (const std::exception& ex) {
            std::cout << "[FAIL] " << tc.name << " (unexpected std::exception)\n";
            std::cout << "       " << ex.what() << "\n";
            ++failed;
        } catch (...) {
            std::cout << "[FAIL] " << tc.name << " (unknown exception)\n";
            ++failed;
        }
    }

    std::cout << "==================================================\n";
    std::cout << "Summary: " << passed << " passed, " << failed << " failed\n";
    std::cout << "==================================================\n";

    return (failed == 0) ? 0 : 1;
}

