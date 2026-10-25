#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace filler::storage {

class BlockDevice {
public:
    static constexpr std::size_t BLOCK_SIZE = 4096;

    explicit BlockDevice(const std::string& path);

    ~BlockDevice();

    BlockDevice(const BlockDevice&) = delete;
    BlockDevice& operator=(const BlockDevice&) = delete;

    BlockDevice(BlockDevice&& other) noexcept;
    BlockDevice& operator=(BlockDevice&& other) noexcept;

    // buffer_size must be >= BLOCK_SIZE.
    void read_block(std::uint64_t block_id, void* buffer, std::size_t buffer_size);

    // buffer_size must be >= BLOCK_SIZE.
    void write_block(std::uint64_t block_id, const void* buffer, std::size_t buffer_size);

    void close();

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint64_t total_blocks() const noexcept;
    [[nodiscard]] std::size_t size_bytes() const noexcept;
    [[nodiscard]] int fd() const noexcept;

private:
    int fd_{-1};
    std::string path_;
    std::uint64_t total_blocks_{0};
    std::size_t size_bytes_{0};
};

} 
