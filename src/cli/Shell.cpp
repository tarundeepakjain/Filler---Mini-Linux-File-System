#include "cli/Shell.hpp"

#include "fs/Formatter.hpp"
#include "fs/Superblock.hpp"

// POSIX / C system headers
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Standard C++ library headers
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace filler::cli {

namespace {

inline bool is_block_all_zeros(const std::uint8_t* data, std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] != 0) {
            return false;
        }
    }
    return true;
}

inline std::uint32_t read_u32_le(const std::uint8_t* src) noexcept {
    std::uint32_t val = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        val |= (static_cast<std::uint32_t>(src[i]) << (i * 8U));
    }
    return val;
}

std::string process_text_argument(const std::string& raw) {
    if (raw.empty()) {
        return "";
    }
    std::string text = raw;
    if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') ||
                             (text.front() == '\'' && text.back() == '\''))) {
        text = text.substr(1, text.size() - 2);
    }

    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            char next = text[i + 1];
            if (next == 'n') {
                result.push_back('\n');
                ++i;
            } else if (next == 'r') {
                result.push_back('\r');
                ++i;
            } else if (next == 't') {
                result.push_back('\t');
                ++i;
            } else if (next == '\\') {
                result.push_back('\\');
                ++i;
            } else if (next == '"') {
                result.push_back('"');
                ++i;
            } else if (next == '\'') {
                result.push_back('\'');
                ++i;
            } else {
                result.push_back(text[i]);
            }
        } else {
            result.push_back(text[i]);
        }
    }
    return result;
}

bool parse_path_and_text(const std::string& args, std::string& path_out, std::string& text_out) {
    std::size_t idx = 0;
    while (idx < args.size() && std::isspace(static_cast<unsigned char>(args[idx]))) {
        ++idx;
    }
    if (idx >= args.size()) {
        return false;
    }

    std::string path;
    if (args[idx] == '"' || args[idx] == '\'') {
        char quote = args[idx++];
        while (idx < args.size() && args[idx] != quote) {
            path.push_back(args[idx++]);
        }
        if (idx < args.size() && args[idx] == quote) {
            ++idx;
        }
    } else {
        while (idx < args.size() && !std::isspace(static_cast<unsigned char>(args[idx]))) {
            path.push_back(args[idx++]);
        }
    }

    while (idx < args.size() && std::isspace(static_cast<unsigned char>(args[idx]))) {
        ++idx;
    }

    std::string raw_text = (idx < args.size()) ? args.substr(idx) : "";
    path_out = path;
    text_out = process_text_argument(raw_text);
    return true;
}

bool parse_write_args(const std::string& args, std::string& path_out, bool& has_offset, std::uint64_t& offset_out, std::string& text_out) {
    std::size_t idx = 0;
    while (idx < args.size() && std::isspace(static_cast<unsigned char>(args[idx]))) {
        ++idx;
    }
    if (idx >= args.size()) {
        return false;
    }

    std::string path;
    if (args[idx] == '"' || args[idx] == '\'') {
        char quote = args[idx++];
        while (idx < args.size() && args[idx] != quote) {
            path.push_back(args[idx++]);
        }
        if (idx < args.size() && args[idx] == quote) {
            ++idx;
        }
    } else {
        while (idx < args.size() && !std::isspace(static_cast<unsigned char>(args[idx]))) {
            path.push_back(args[idx++]);
        }
    }

    while (idx < args.size() && std::isspace(static_cast<unsigned char>(args[idx]))) {
        ++idx;
    }

    has_offset = false;
    offset_out = 0;

    if (idx < args.size() && std::isdigit(static_cast<unsigned char>(args[idx]))) {
        std::size_t num_end = idx;
        while (num_end < args.size() && std::isdigit(static_cast<unsigned char>(args[num_end]))) {
            ++num_end;
        }
        std::size_t text_start = num_end;
        while (text_start < args.size() && std::isspace(static_cast<unsigned char>(args[text_start]))) {
            ++text_start;
        }
        if (text_start < args.size()) {
            try {
                offset_out = std::stoull(args.substr(idx, num_end - idx));
                has_offset = true;
                idx = text_start;
            } catch (...) {
                has_offset = false;
            }
        }
    }

    std::string raw_text = (idx < args.size()) ? args.substr(idx) : "";
    path_out = path;
    text_out = process_text_argument(raw_text);
    return true;
}

} // namespace

Shell::Shell(const std::string& image_path)
    : image_path_(image_path) {
    struct stat st{};
    const bool exists = (::stat(image_path_.c_str(), &st) == 0);
    if (!exists) {
        int fd = ::open(image_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fs_state_ = FilesystemState::IO_ERROR;
            is_formatted_ = false;
            status_message_ = "Disk I/O error: Failed to create disk image: " + image_path_;
            return;
        }
        if (::ftruncate(fd, static_cast<off_t>(DEFAULT_IMAGE_SIZE)) != 0) {
            int err = errno;
            ::close(fd);
            fs_state_ = FilesystemState::IO_ERROR;
            is_formatted_ = false;
            status_message_ = "Disk I/O error: Failed to allocate disk image size: " + std::string(strerror(err));
            return;
        }
        ::close(fd);
    }

    try {
        device_ = std::make_unique<filler::storage::BlockDevice>(image_path_);
    } catch (const std::exception& ex) {
        device_.reset();
        fs_state_ = FilesystemState::IO_ERROR;
        is_formatted_ = false;
        status_message_ = "Disk I/O error opening image: " + std::string(ex.what());
        return;
    }

    inspect_filesystem();
}

void Shell::inspect_filesystem() {
    is_formatted_ = false;
    current_dir_inode_ = filler::fs::Superblock::DEFAULT_ROOT_INODE;
    current_path_ = "/";

    if (!device_ || !device_->is_open()) {
        fs_state_ = FilesystemState::IO_ERROR;
        status_message_ = "Disk I/O error: Block device is not open";
        return;
    }

    std::vector<std::uint8_t> block0(filler::storage::BlockDevice::BLOCK_SIZE, 0);
    try {
        device_->read_block(filler::fs::Superblock::SUPERBLOCK_BLOCK_ID, block0.data(), block0.size());
    } catch (const std::exception& ex) {
        fs_state_ = FilesystemState::IO_ERROR;
        status_message_ = "Disk I/O error reading superblock: " + std::string(ex.what());
        return;
    }

    const std::uint32_t magic = read_u32_le(block0.data() + filler::fs::offsets::MAGIC);
    const std::uint32_t version = read_u32_le(block0.data() + filler::fs::offsets::VERSION);

    // Case 2: Unformatted / newly created disk image vs existing image with erased magic
    if (magic == 0) {
        const bool block0_all_zeros = is_block_all_zeros(block0.data(), block0.size());
        if (!block0_all_zeros) {
            // Magic is 0, but other fields in block 0 are non-zero -> magic was erased/corrupted
            fs_state_ = FilesystemState::CORRUPTED;
            status_message_ = "Corrupted filesystem: superblock magic number is zero/missing on an image with existing metadata.";
            return;
        }

        // Even if block 0 is all zeros, check if block 1 (block bitmap) or block 2 (inode bitmap) contains existing data
        std::vector<std::uint8_t> check_buf(filler::storage::BlockDevice::BLOCK_SIZE, 0);
        bool existing_data_found = false;
        try {
            device_->read_block(1, check_buf.data(), check_buf.size());
            if (!is_block_all_zeros(check_buf.data(), check_buf.size())) {
                existing_data_found = true;
            } else {
                device_->read_block(2, check_buf.data(), check_buf.size());
                if (!is_block_all_zeros(check_buf.data(), check_buf.size())) {
                    existing_data_found = true;
                }
            }
        } catch (...) {
            // Ignore read errors here
        }

        if (existing_data_found) {
            fs_state_ = FilesystemState::CORRUPTED;
            status_message_ = "Corrupted filesystem: superblock is erased/empty, but existing filesystem structures are present.";
            return;
        }

        // Genuinely blank / fresh all-zero image
        fs_state_ = FilesystemState::UNFORMATTED;
        status_message_ = "Filesystem is unformatted. Run 'format' to initialize.";
        return;
    }

    // Case 3: Invalid filesystem image (unrecognized magic or unsupported version)
    if (magic != filler::fs::Superblock::MAGIC) {
        fs_state_ = FilesystemState::INVALID_IMAGE;
        std::ostringstream oss;
        oss << "Invalid filesystem image: unrecognized magic number 0x"
            << std::hex << std::uppercase << magic << " (expected 0x"
            << std::hex << std::uppercase << filler::fs::Superblock::MAGIC << ")";
        status_message_ = oss.str();
        return;
    }

    if (version != filler::fs::Superblock::FORMAT_VERSION) {
        fs_state_ = FilesystemState::INVALID_IMAGE;
        status_message_ = "Invalid filesystem image: unsupported version " +
                          std::to_string(version) + " (expected " +
                          std::to_string(filler::fs::Superblock::FORMAT_VERSION) + ")";
        return;
    }

    // Case 4: Corrupted filesystem metadata
    filler::fs::Superblock sb;
    try {
        sb = filler::fs::Superblock::deserialize(block0.data(), block0.size(), true);
    } catch (const std::exception& ex) {
        fs_state_ = FilesystemState::CORRUPTED;
        status_message_ = "Corrupted filesystem metadata: " + std::string(ex.what());
        return;
    }

    if (sb.total_blocks != device_->total_blocks()) {
        fs_state_ = FilesystemState::CORRUPTED;
        status_message_ = "Corrupted filesystem metadata: Superblock total_blocks (" +
                          std::to_string(sb.total_blocks) + ") does not match physical device blocks (" +
                          std::to_string(device_->total_blocks()) + ")";
        return;
    }

    try {
        init_subsystems();
        if (!inode_manager_->is_inode_allocated(sb.root_inode)) {
            throw std::runtime_error("Root inode " + std::to_string(sb.root_inode) + " is not allocated");
        }
        filler::fs::Inode root_inode = inode_manager_->get_inode(sb.root_inode);
        if (!root_inode.is_directory()) {
            throw std::runtime_error("Root inode " + std::to_string(sb.root_inode) + " is not a directory");
        }
    } catch (const std::exception& ex) {
        fs_state_ = FilesystemState::CORRUPTED;
        status_message_ = "Corrupted filesystem metadata in subsystem initialization: " + std::string(ex.what());
        block_bitmap_.reset();
        inode_manager_.reset();
        directory_manager_.reset();
        file_manager_.reset();
        return;
    }

    // Case 1: Valid formatted filesystem
    fs_state_ = FilesystemState::VALID;
    is_formatted_ = true;
    status_message_ = "Filesystem is valid and mounted.";
}

void Shell::init_subsystems() {
    block_bitmap_ = std::make_unique<filler::fs::BlockBitmap>(*device_);
    inode_manager_ = std::make_unique<filler::fs::InodeManager>(*device_);
    directory_manager_ = std::make_unique<filler::fs::DirectoryManager>(*device_, *inode_manager_, *block_bitmap_);
    file_manager_ = std::make_unique<filler::fs::FileManager>(*device_, *inode_manager_, *block_bitmap_, *directory_manager_);
}

void Shell::format_filesystem(std::ostream& out) {
    if (!device_ || device_->total_blocks() != filler::fs::Formatter::DEFAULT_TOTAL_BLOCKS) {
        if (device_) {
            device_->close();
        }
        int fd = ::open(image_path_.c_str(), O_RDWR);
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "Failed to open disk image: " + image_path_);
        }
        if (::ftruncate(fd, static_cast<off_t>(DEFAULT_IMAGE_SIZE)) != 0) {
            int err = errno;
            ::close(fd);
            throw std::system_error(err, std::generic_category(), "Failed to resize disk image: " + image_path_);
        }
        ::close(fd);
        device_ = std::make_unique<filler::storage::BlockDevice>(image_path_);
    }

    filler::fs::Formatter::format(*device_);
    init_subsystems();
    directory_manager_->initialize_root();
    inspect_filesystem();
    out << "Filesystem successfully formatted.\n";
}

std::uint64_t Shell::resolve_path(const std::string& path, std::string* canonical_out) {
    if (!is_formatted_) {
        if (fs_state_ == FilesystemState::UNFORMATTED) {
            throw std::runtime_error("Filesystem is not formatted");
        }
        throw std::runtime_error(status_message_);
    }

    if (path.empty()) {
        if (canonical_out != nullptr) {
            *canonical_out = current_path_;
        }
        return current_dir_inode_;
    }

    std::uint64_t curr_inode = current_dir_inode_;
    std::vector<std::string> components;

    if (path.front() == '/') {
        curr_inode = filler::fs::Superblock::DEFAULT_ROOT_INODE;
    } else {
        std::stringstream ss(current_path_);
        std::string seg;
        while (std::getline(ss, seg, '/')) {
            if (!seg.empty()) {
                components.push_back(seg);
            }
        }
    }

    std::stringstream ss(path);
    std::string token;
    while (std::getline(ss, token, '/')) {
        if (token.empty() || token == ".") {
            continue;
        }
        if (token == "..") {
            curr_inode = directory_manager_->lookup(curr_inode, "..");
            if (!components.empty()) {
                components.pop_back();
            }
        } else {
            curr_inode = directory_manager_->lookup(curr_inode, token);
            components.push_back(token);
        }
    }

    if (canonical_out != nullptr) {
        if (components.empty()) {
            *canonical_out = "/";
        } else {
            std::string res;
            for (const auto& comp : components) {
                res += "/" + comp;
            }
            *canonical_out = res;
        }
    }

    return curr_inode;
}

std::pair<std::string, std::string> Shell::split_path(const std::string& path) {
    if (path.empty()) {
        return {"", ""};
    }

    std::string trimmed = path;
    while (trimmed.size() > 1 && trimmed.back() == '/') {
        trimmed.pop_back();
    }

    if (trimmed == "/") {
        return {"/", ""};
    }

    const auto last_slash = trimmed.find_last_of('/');
    if (last_slash == std::string::npos) {
        return {"", trimmed};
    }

    if (last_slash == 0) {
        return {"/", trimmed.substr(1)};
    }

    return {trimmed.substr(0, last_slash), trimmed.substr(last_slash + 1)};
}

void Shell::cmd_format(std::ostream& out) {
    format_filesystem(out);
}

void Shell::cmd_pwd(std::ostream& out) {
    out << current_path_ << "\n";
}

void Shell::cmd_cd(const std::string& args, std::ostream& out) {
    const std::string target = args.empty() ? "/" : args;
    std::string canonical;
    const std::uint64_t target_inode = resolve_path(target, &canonical);
    const filler::fs::Inode inode = inode_manager_->get_inode(target_inode);
    if (!inode.is_directory()) {
        out << "Error: Not a directory: " << target << "\n";
        return;
    }
    current_dir_inode_ = target_inode;
    current_path_ = canonical;
}

void Shell::cmd_ls(const std::string& args, std::ostream& out) {
    const std::uint64_t target_inode = args.empty() ? current_dir_inode_ : resolve_path(args);
    const filler::fs::Inode inode = inode_manager_->get_inode(target_inode);
    if (inode.is_directory()) {
        const auto entries = directory_manager_->list(target_inode);
        for (const auto& entry : entries) {
            const filler::fs::Inode entry_inode = inode_manager_->get_inode(entry.inode_id);
            if (entry_inode.is_directory()) {
                out << "[DIR]  " << entry.name << "\n";
            } else {
                out << "[FILE] " << entry.name << " (" << entry_inode.size_bytes << " bytes)\n";
            }
        }
    } else {
        out << "[FILE] " << (args.empty() ? current_path_ : args) << " (" << inode.size_bytes << " bytes)\n";
    }
}

void Shell::cmd_mkdir(const std::string& args, std::ostream& out) {
    if (args.empty()) {
        out << "Error: mkdir requires a path\n";
        return;
    }
    auto [parent_path, name] = split_path(args);
    if (name.empty()) {
        out << "Error: Invalid directory name\n";
        return;
    }
    const std::uint64_t parent_inode = parent_path.empty() ? current_dir_inode_ : resolve_path(parent_path);
    directory_manager_->mkdir(parent_inode, name);
}

void Shell::cmd_touch(const std::string& args, std::ostream& out) {
    if (args.empty()) {
        out << "Error: touch requires a path\n";
        return;
    }
    auto [parent_path, name] = split_path(args);
    if (name.empty()) {
        out << "Error: Invalid file name\n";
        return;
    }
    const std::uint64_t parent_inode = parent_path.empty() ? current_dir_inode_ : resolve_path(parent_path);
    try {
        const std::uint64_t existing_inode = directory_manager_->lookup(parent_inode, name);
        filler::fs::Inode inode = inode_manager_->get_inode(existing_inode);
        inode.mtime_sec = static_cast<std::int64_t>(std::time(nullptr));
        inode.atime_sec = inode.mtime_sec;
        inode_manager_->update_inode(existing_inode, inode);
    } catch (const filler::fs::EntryNotFoundError&) {
        file_manager_->create(parent_inode, name);
    }
}

void Shell::cmd_cat(const std::string& args, std::ostream& out) {
    if (args.empty()) {
        out << "Error: cat requires a path\n";
        return;
    }
    const std::uint64_t file_inode = resolve_path(args);
    const filler::fs::Inode inode = inode_manager_->get_inode(file_inode);
    if (inode.is_directory()) {
        out << "Error: Is a directory: " << args << "\n";
        return;
    }
    if (inode.size_bytes > 0) {
        const auto data = file_manager_->read(file_inode, 0, inode.size_bytes);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!data.empty() && data.back() != '\n') {
            out << "\n";
        }
    }
}

void Shell::cmd_write(const std::string& args, std::ostream& out) {
    std::string path, text;
    bool has_offset = false;
    std::uint64_t offset = 0;
    if (!parse_write_args(args, path, has_offset, offset, text) || path.empty()) {
        out << "Error: write requires a path and content\n";
        return;
    }
    auto [parent_path, name] = split_path(path);
    if (name.empty()) {
        out << "Error: Invalid file name\n";
        return;
    }
    const std::uint64_t parent_inode = parent_path.empty() ? current_dir_inode_ : resolve_path(parent_path);
    std::uint64_t file_inode = 0;
    try {
        file_inode = directory_manager_->lookup(parent_inode, name);
        const filler::fs::Inode inode = inode_manager_->get_inode(file_inode);
        if (inode.is_directory()) {
            out << "Error: Cannot write to directory: " << path << "\n";
            return;
        }
    } catch (const filler::fs::EntryNotFoundError&) {
        file_inode = file_manager_->create(parent_inode, name);
    }

    if (!has_offset) {
        file_manager_->truncate(file_inode, 0);
    }
    if (!text.empty()) {
        file_manager_->write(file_inode, offset, text);
    }
}

void Shell::cmd_truncate(const std::string& args, std::ostream& out) {
    std::size_t idx = 0;
    while (idx < args.size() && std::isspace(static_cast<unsigned char>(args[idx]))) {
        ++idx;
    }
    if (idx >= args.size()) {
        out << "Error: truncate requires a path and size\n";
        return;
    }

    std::string path;
    if (args[idx] == '"' || args[idx] == '\'') {
        char quote = args[idx++];
        while (idx < args.size() && args[idx] != quote) {
            path.push_back(args[idx++]);
        }
        if (idx < args.size() && args[idx] == quote) {
            ++idx;
        }
    } else {
        while (idx < args.size() && !std::isspace(static_cast<unsigned char>(args[idx]))) {
            path.push_back(args[idx++]);
        }
    }

    while (idx < args.size() && std::isspace(static_cast<unsigned char>(args[idx]))) {
        ++idx;
    }

    if (idx >= args.size()) {
        out << "Error: truncate requires a size\n";
        return;
    }

    const std::string size_str = args.substr(idx);
    std::uint64_t new_size = 0;
    try {
        new_size = std::stoull(size_str);
    } catch (...) {
        out << "Error: Invalid size: " << size_str << "\n";
        return;
    }

    const std::uint64_t target_inode = resolve_path(path);
    file_manager_->truncate(target_inode, new_size);
}

void Shell::cmd_append(const std::string& args, std::ostream& out) {
    std::string path, text;
    if (!parse_path_and_text(args, path, text) || path.empty()) {
        out << "Error: append requires a path and content\n";
        return;
    }
    auto [parent_path, name] = split_path(path);
    if (name.empty()) {
        out << "Error: Invalid file name\n";
        return;
    }
    const std::uint64_t parent_inode = parent_path.empty() ? current_dir_inode_ : resolve_path(parent_path);
    std::uint64_t file_inode = 0;
    try {
        file_inode = directory_manager_->lookup(parent_inode, name);
        const filler::fs::Inode inode = inode_manager_->get_inode(file_inode);
        if (inode.is_directory()) {
            out << "Error: Cannot append to directory: " << path << "\n";
            return;
        }
    } catch (const filler::fs::EntryNotFoundError&) {
        file_inode = file_manager_->create(parent_inode, name);
    }

    if (!text.empty()) {
        file_manager_->append(file_inode, text);
    }
}

void Shell::cmd_rm(const std::string& args, std::ostream& out) {
    if (args.empty()) {
        out << "Error: rm requires a path\n";
        return;
    }
    auto [parent_path, name] = split_path(args);
    if (name.empty()) {
        out << "Error: Invalid file name\n";
        return;
    }
    const std::uint64_t parent_inode = parent_path.empty() ? current_dir_inode_ : resolve_path(parent_path);
    const std::uint64_t target_inode = directory_manager_->lookup(parent_inode, name);
    const filler::fs::Inode inode = inode_manager_->get_inode(target_inode);
    if (inode.is_directory()) {
        out << "Error: Cannot remove directory with rm (use rmdir): " << name << "\n";
        return;
    }
    file_manager_->remove(parent_inode, name);
}

void Shell::cmd_rmdir(const std::string& args, std::ostream& out) {
    if (args.empty()) {
        out << "Error: rmdir requires a path\n";
        return;
    }
    auto [parent_path, name] = split_path(args);
    if (name.empty() || name == "." || name == "..") {
        out << "Error: Cannot remove '" << args << "'\n";
        return;
    }
    const std::uint64_t parent_inode = parent_path.empty() ? current_dir_inode_ : resolve_path(parent_path);
    directory_manager_->remove_directory(parent_inode, name);
}

void Shell::cmd_stat(const std::string& args, std::ostream& out) {
    if (args.empty()) {
        out << "Error: stat requires a path\n";
        return;
    }
    const std::uint64_t target_inode = resolve_path(args);
    const filler::fs::Inode inode = inode_manager_->get_inode(target_inode);
    out << "Inode: " << inode.inode_id << "\n";
    out << "Type: " << (inode.type == filler::fs::FileType::DIRECTORY ? "Directory" :
                       inode.type == filler::fs::FileType::REGULAR ? "Regular File" : "Unknown") << "\n";
    out << "Size: " << inode.size_bytes << " bytes\n";
    out << "Blocks: " << inode.blocks_count << "\n";
    out << "Permissions: 0" << std::oct << inode.permissions << std::dec << "\n";
    out << "UID: " << inode.uid << ", GID: " << inode.gid << "\n";
    out << "Direct blocks: [";
    bool first = true;
    for (std::size_t i = 0; i < filler::fs::Inode::DIRECT_BLOCKS_COUNT; ++i) {
        if (inode.direct_blocks[i] != 0) {
            if (!first) {
                out << ", ";
            }
            out << i << ":" << inode.direct_blocks[i];
            first = false;
        }
    }
    out << "]\n";
    if (inode.single_indirect != 0) {
        out << "Single indirect: " << inode.single_indirect << "\n";
    }
    if (inode.double_indirect != 0) {
        out << "Double indirect: " << inode.double_indirect << "\n";
    }
    out << "Access time: " << inode.atime_sec << "\n";
    out << "Modify time: " << inode.mtime_sec << "\n";
    out << "Change time: " << inode.ctime_sec << "\n";
}

void Shell::cmd_help(std::ostream& out) {
    out << "Available commands:\n";
    out << "  format                    Format the disk image\n";
    out << "  mkdir <path>              Create a directory\n";
    out << "  touch <path>              Create an empty file\n";
    out << "  ls [path]                 List directory contents\n";
    out << "  cd <path>                 Change current directory\n";
    out << "  pwd                       Display current directory\n";
    out << "  cat <path>                Display file contents\n";
    out << "  write <path> [off] <text> Write content to a file (optional byte offset)\n";
    out << "  append <path> <text>      Append content to a file\n";
    out << "  truncate <path> <size>    Truncate file to specified size\n";
    out << "  rm <path>                 Remove a file\n";
    out << "  rmdir <path>              Remove an empty directory\n";
    out << "  stat <path>               Display inode and file metadata\n";
    out << "  help                      Display available commands\n";
    out << "  exit                      Exit the shell\n";
}

bool Shell::execute_command(const std::string& cmd_line, std::ostream& out) {
    std::size_t start = 0;
    while (start < cmd_line.size() && std::isspace(static_cast<unsigned char>(cmd_line[start]))) {
        ++start;
    }
    if (start >= cmd_line.size()) {
        return true;
    }

    std::size_t end = start;
    while (end < cmd_line.size() && !std::isspace(static_cast<unsigned char>(cmd_line[end]))) {
        ++end;
    }

    const std::string cmd = cmd_line.substr(start, end - start);

    while (end < cmd_line.size() && std::isspace(static_cast<unsigned char>(cmd_line[end]))) {
        ++end;
    }
    std::string args;
    if (end < cmd_line.size()) {
        args = cmd_line.substr(end);
        while (!args.empty() && std::isspace(static_cast<unsigned char>(args.back()))) {
            args.pop_back();
        }
    }

    if (cmd == "exit" || cmd == "quit") {
        return false;
    }

    try {
        if (cmd == "help") {
            cmd_help(out);
        } else if (cmd == "format") {
            cmd_format(out);
        } else {
            if (!is_formatted_) {
                if (fs_state_ == FilesystemState::UNFORMATTED) {
                    out << "Error: Filesystem is not formatted. Run 'format' first.\n";
                } else {
                    out << "Error: " << status_message_ << "\n";
                }
                return true;
            }

            if (cmd == "pwd") {
                cmd_pwd(out);
            } else if (cmd == "cd") {
                cmd_cd(args, out);
            } else if (cmd == "ls") {
                cmd_ls(args, out);
            } else if (cmd == "mkdir") {
                cmd_mkdir(args, out);
            } else if (cmd == "touch") {
                cmd_touch(args, out);
            } else if (cmd == "cat") {
                cmd_cat(args, out);
            } else if (cmd == "write") {
                cmd_write(args, out);
            } else if (cmd == "append") {
                cmd_append(args, out);
            } else if (cmd == "truncate") {
                cmd_truncate(args, out);
            } else if (cmd == "rm") {
                cmd_rm(args, out);
            } else if (cmd == "rmdir") {
                cmd_rmdir(args, out);
            } else if (cmd == "stat") {
                cmd_stat(args, out);
            } else {
                out << "Unknown command: " << cmd << ". Type 'help' for available commands.\n";
            }
        }
    } catch (const std::exception& ex) {
        out << "Error: " << ex.what() << "\n";
    }

    return true;
}

void Shell::run(std::istream& in, std::ostream& out) {
    std::string line;
    while (true) {
        out << "filler:" << current_path_ << "> " << std::flush;
        if (!std::getline(in, line)) {
            break;
        }
        if (!execute_command(line, out)) {
            break;
        }
    }
}

} // namespace filler::cli
