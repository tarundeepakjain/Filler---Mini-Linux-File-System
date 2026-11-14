#pragma once

#include "fs/BlockBitmap.hpp"
#include "fs/DirectoryManager.hpp"
#include "fs/FileManager.hpp"
#include "fs/Formatter.hpp"
#include "fs/Inode.hpp"
#include "fs/InodeManager.hpp"
#include "fs/Superblock.hpp"
#include "storage/BlockDevice.hpp"

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace filler::cli {

class Shell {
public:
    static constexpr std::size_t DEFAULT_IMAGE_SIZE = 64ULL * 1024ULL * 1024ULL; // 64 MiB

    // Constructs the shell targeting the disk image at image_path.
    // If image_path does not exist, creates the 64 MiB empty file.
    explicit Shell(const std::string& image_path);

    // Executes a single command line string and writes output to out.
    // Returns false if the command was 'exit', true otherwise.
    bool execute_command(const std::string& cmd_line, std::ostream& out);

    // Launches the interactive REPL loop reading from in and writing to out.
    void run(std::istream& in, std::ostream& out);

    // Explicitly formats the filesystem and initializes root directory.
    void format_filesystem(std::ostream& out);

    // Accessors
    [[nodiscard]] bool is_formatted() const noexcept { return is_formatted_; }
    [[nodiscard]] std::uint64_t current_dir_inode() const noexcept { return current_dir_inode_; }
    [[nodiscard]] const std::string& current_path() const noexcept { return current_path_; }

    // Path resolution helper
    [[nodiscard]] std::uint64_t resolve_path(const std::string& path, std::string* canonical_out = nullptr);

private:
    std::string image_path_;
    std::unique_ptr<filler::storage::BlockDevice> device_;
    std::unique_ptr<filler::fs::BlockBitmap> block_bitmap_;
    std::unique_ptr<filler::fs::InodeManager> inode_manager_;
    std::unique_ptr<filler::fs::DirectoryManager> directory_manager_;
    std::unique_ptr<filler::fs::FileManager> file_manager_;

    bool is_formatted_{false};
    std::uint64_t current_dir_inode_{1};
    std::string current_path_{"/"};

    void init_subsystems();
    [[nodiscard]] std::pair<std::string, std::string> split_path(const std::string& path);

    // Command handlers
    void cmd_format(std::ostream& out);
    void cmd_pwd(std::ostream& out);
    void cmd_cd(const std::string& args, std::ostream& out);
    void cmd_ls(const std::string& args, std::ostream& out);
    void cmd_mkdir(const std::string& args, std::ostream& out);
    void cmd_touch(const std::string& args, std::ostream& out);
    void cmd_cat(const std::string& args, std::ostream& out);
    void cmd_write(const std::string& args, std::ostream& out);
    void cmd_append(const std::string& args, std::ostream& out);
    void cmd_rm(const std::string& args, std::ostream& out);
    void cmd_rmdir(const std::string& args, std::ostream& out);
    void cmd_stat(const std::string& args, std::ostream& out);
    void cmd_help(std::ostream& out);
};

} // namespace filler::cli

