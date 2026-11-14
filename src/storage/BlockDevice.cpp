#include "storage/BlockDevice.hpp"

// Linux / POSIX system headers
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#elif defined(__APPLE__)
#include <sys/disk.h>
#include <sys/ioctl.h>
#endif

// C++ standard library headers
#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>


namespace filler::storage {

BlockDevice::BlockDevice(const std::string &path)
    : path_(path) {
    // Open the existing image file or block device for reading and writing.
    // Notice O_CREAT is intentionally omitted to avoid silently creating a new/empty file.
#if defined(O_CLOEXEC)
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
#else
    fd_ = ::open(path.c_str(), O_RDWR);
#endif

    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "Failed to open image file or block device: " + path);
    }

    // Inspect storage metadata
    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
        int saved_errno = errno;
        ::close(fd_);
        fd_ = -1;
        throw std::system_error(saved_errno, std::generic_category(), "Failed to stat storage target: " + path);
    }

    std::uint64_t detected_size = 0;

    if (S_ISREG(st.st_mode)) {
        is_block_device_ = false;
        if (st.st_size <= 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("Backing storage file is empty: " + path);
        }
        detected_size = static_cast<std::uint64_t>(st.st_size);
    } else if (S_ISBLK(st.st_mode)) {
        is_block_device_ = true;
#if defined(__linux__) && defined(BLKGETSIZE64)
        std::uint64_t blk_bytes = 0;
        if (::ioctl(fd_, BLKGETSIZE64, &blk_bytes) != 0 || blk_bytes == 0) {
            int err = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::system_error(err, std::generic_category(), "Failed to query Linux block device capacity (BLKGETSIZE64): " + path);
        }
        detected_size = blk_bytes;
#elif defined(__APPLE__) && defined(DKIOCGETBLOCKCOUNT) && defined(DKIOCGETBLOCKSIZE)
        std::uint64_t block_count = 0;
        std::uint32_t block_size = 0;
        if (::ioctl(fd_, DKIOCGETBLOCKCOUNT, &block_count) != 0 ||
            ::ioctl(fd_, DKIOCGETBLOCKSIZE, &block_size) != 0 ||
            block_count == 0 || block_size == 0) {
            int err = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::system_error(err, std::generic_category(), "Failed to query macOS disk device capacity: " + path);
        }
        detected_size = block_count * static_cast<std::uint64_t>(block_size);
#else
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Block device querying not supported on this platform: " + path);
#endif
    } else {
        ::close(fd_);
        fd_ = -1;
        if (S_ISDIR(st.st_mode)) {
            throw std::runtime_error("Backing storage is a directory, not a regular file or block device: " + path);
        }
        throw std::runtime_error("Unsupported device type: " + path + " (must be regular file or block device)");
    }

    // Ensure size is a multiple of BLOCK_SIZE (4096 bytes)
    if (detected_size % BLOCK_SIZE != 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Backing storage size (" + std::to_string(detected_size) +
                                 " bytes) is not an exact multiple of BLOCK_SIZE (4096 bytes): " + path);
    }

    size_bytes_ = detected_size;
    total_blocks_ = size_bytes_ / BLOCK_SIZE;
}

BlockDevice::~BlockDevice() {
    close();
}

BlockDevice::BlockDevice(BlockDevice&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      path_(std::move(other.path_)),
      total_blocks_(std::exchange(other.total_blocks_, 0)),
      size_bytes_(std::exchange(other.size_bytes_, 0)),
      is_block_device_(std::exchange(other.is_block_device_, false)) {}

BlockDevice& BlockDevice::operator=(BlockDevice&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
        path_ = std::move(other.path_);
        total_blocks_ = std::exchange(other.total_blocks_, 0);
        size_bytes_ = std::exchange(other.size_bytes_, 0);
        is_block_device_ = std::exchange(other.is_block_device_, false);
    }
    return *this;
}

void BlockDevice::close() {
    if (fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }
}

void BlockDevice::read_block(std::uint64_t block_id, void* buffer, std::size_t buffer_size) {
    if (!is_open()) {
        throw std::runtime_error("Cannot read block: BlockDevice is not open");
    }

    if (block_id >= total_blocks_) {
        throw std::out_of_range("Block ID " + std::to_string(block_id) +
                                " is out of range. Total blocks available: " + std::to_string(total_blocks_));
    }

    if (buffer == nullptr) {
        throw std::invalid_argument("Cannot read block: buffer pointer is null");
    }

    if (buffer_size < BLOCK_SIZE) {
        throw std::invalid_argument("Buffer size (" + std::to_string(buffer_size) +
                                    ") is smaller than BLOCK_SIZE (" + std::to_string(BLOCK_SIZE) + ")");
    }

    const auto base_offset = static_cast<off_t>(block_id * BLOCK_SIZE);
    auto* dest = static_cast<char*>(buffer);
    std::size_t bytes_read = 0;

    // Loop to handle partial reads and EINTR
    while (bytes_read < BLOCK_SIZE) {
        const off_t current_offset = base_offset + static_cast<off_t>(bytes_read);
        const std::size_t remaining = BLOCK_SIZE - bytes_read;

        const ssize_t ret = ::pread(fd_, dest + bytes_read, remaining, current_offset);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(),
                                    "pread failed on block " + std::to_string(block_id));
        }

        if (ret == 0) {
            // Unexpected EOF encountered before completing the block
            throw std::runtime_error("Unexpected EOF while reading block " + std::to_string(block_id) +
                                     " (read " + std::to_string(bytes_read) + " of " +
                                     std::to_string(BLOCK_SIZE) + " bytes)");
        }

        bytes_read += static_cast<std::size_t>(ret);
    }
}

void BlockDevice::write_block(std::uint64_t block_id, const void* buffer, std::size_t buffer_size) {
    if (!is_open()) {
        throw std::runtime_error("Cannot write block: BlockDevice is not open");
    }

    if (block_id >= total_blocks_) {
        throw std::out_of_range("Block ID " + std::to_string(block_id) +
                                " is out of range. Total blocks available: " + std::to_string(total_blocks_));
    }

    if (buffer == nullptr) {
        throw std::invalid_argument("Cannot write block: buffer pointer is null");
    }

    if (buffer_size < BLOCK_SIZE) {
        throw std::invalid_argument("Buffer size (" + std::to_string(buffer_size) +
                                    ") is smaller than BLOCK_SIZE (" + std::to_string(BLOCK_SIZE) + ")");
    }

    const auto base_offset = static_cast<off_t>(block_id * BLOCK_SIZE);
    const auto* src = static_cast<const char*>(buffer);
    std::size_t bytes_written = 0;

    // Loop to handle partial writes and EINTR
    while (bytes_written < BLOCK_SIZE) {
        const off_t current_offset = base_offset + static_cast<off_t>(bytes_written);
        const std::size_t remaining = BLOCK_SIZE - bytes_written;

        const ssize_t ret = ::pwrite(fd_, src + bytes_written, remaining, current_offset);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(),
                                    "pwrite failed on block " + std::to_string(block_id));
        }

        if (ret == 0) {
            throw std::runtime_error("Zero bytes written while writing block " + std::to_string(block_id) +
                                     " (wrote " + std::to_string(bytes_written) + " of " +
                                     std::to_string(BLOCK_SIZE) + " bytes)");
        }

        bytes_written += static_cast<std::size_t>(ret);
    }
}

bool BlockDevice::is_open() const noexcept {
    return fd_ != -1;
}

std::uint64_t BlockDevice::total_blocks() const noexcept {
    return total_blocks_;
}

std::uint64_t BlockDevice::size_bytes() const noexcept {
    return size_bytes_;
}

int BlockDevice::fd() const noexcept {
    return fd_;
}

bool BlockDevice::is_block_device() const noexcept {
    return is_block_device_;
}

const std::string& BlockDevice::path() const noexcept {
    return path_;
}

} // namespace filler::storage
