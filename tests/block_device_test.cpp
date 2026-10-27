#include "storage/BlockDevice.hpp"

// POSIX and system headers
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// C++ standard library
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

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
    if constexpr (std::is_same_v<ExpectedException, std::exception>) {
        try {
            fn();
        } catch (const std::exception&) {
            caught = true;
        } catch (...) {
            throw TestFailure(std::string("Expected ") + expected_type_str + " for " + expr_str +
                              ", but caught non-standard exception", file, line);
        }
    } else {
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
    assert_throws_impl<ExceptionType>([&]() { expr; }, #expr, #ExceptionType, __FILE__, __LINE__)

// ============================================================================
// Disposable Test Image Management (RAII)
// ============================================================================

void create_binary_file(const std::string& path, std::size_t size_bytes, uint8_t fill_byte = 0) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        throw std::runtime_error("Failed to create temporary file: " + path);
    }
    if (size_bytes > 0) {
        constexpr std::size_t CHUNK_SIZE = 4096;
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

// Deterministic block pattern generator
std::vector<uint8_t> make_block_pattern(std::uint64_t block_id, uint8_t seed = 0x5A) {
    std::vector<uint8_t> block(BlockDevice::BLOCK_SIZE);
    for (std::size_t i = 0; i < block.size(); ++i) {
        block[i] = static_cast<uint8_t>((seed + (block_id * 37) + (i * 19)) & 0xFF);
    }
    return block;
}

// ============================================================================
// Test Suite Cases
// ============================================================================

// 1. Opening a valid, non-empty image file.
void test_open_valid_image() {
    ScopedTestFile file("build/test_open_valid.img");
    file.create(BlockDevice::BLOCK_SIZE * 2, 0xAA);

    BlockDevice device(file.path());
    ASSERT_TRUE(device.is_open());
    ASSERT_TRUE(device.fd() >= 0);
}

// 2. Correct image size and total block count.
void test_image_size_and_total_blocks() {
    // 1 block
    {
        ScopedTestFile file("build/test_size_1blk.img");
        file.create(BlockDevice::BLOCK_SIZE, 0x00);
        BlockDevice device(file.path());
        ASSERT_EQ(device.size_bytes(), BlockDevice::BLOCK_SIZE);
        ASSERT_EQ(device.total_blocks(), static_cast<std::uint64_t>(1));
    }
    // 4 blocks
    {
        ScopedTestFile file("build/test_size_4blks.img");
        file.create(BlockDevice::BLOCK_SIZE * 4, 0x00);
        BlockDevice device(file.path());
        ASSERT_EQ(device.size_bytes(), BlockDevice::BLOCK_SIZE * 4);
        ASSERT_EQ(device.total_blocks(), static_cast<std::uint64_t>(4));
    }
    // 16 blocks (64 KiB)
    {
        ScopedTestFile file("build/test_size_16blks.img");
        file.create(BlockDevice::BLOCK_SIZE * 16, 0x00);
        BlockDevice device(file.path());
        ASSERT_EQ(device.size_bytes(), BlockDevice::BLOCK_SIZE * 16);
        ASSERT_EQ(device.total_blocks(), static_cast<std::uint64_t>(16));
    }
}

// 3. Writing a complete block and reading it back.
void test_write_and_read_single_block() {
    ScopedTestFile file("build/test_rw_single.img");
    file.create(BlockDevice::BLOCK_SIZE * 2, 0x00);

    BlockDevice device(file.path());
    const auto pattern = make_block_pattern(0, 0x33);
    std::vector<uint8_t> read_buf(BlockDevice::BLOCK_SIZE, 0x00);

    device.write_block(0, pattern.data(), pattern.size());
    device.read_block(0, read_buf.data(), read_buf.size());

    ASSERT_EQ(std::memcmp(read_buf.data(), pattern.data(), BlockDevice::BLOCK_SIZE), 0);
}

// 4. Reading and writing multiple different blocks independently.
void test_read_write_multiple_blocks_independently() {
    ScopedTestFile file("build/test_rw_multi.img");
    file.create(BlockDevice::BLOCK_SIZE * 4, 0x00);

    BlockDevice device(file.path());
    const auto p0 = make_block_pattern(0, 0x11);
    const auto p1 = make_block_pattern(1, 0x22);
    const auto p2 = make_block_pattern(2, 0x33);
    const auto p3 = make_block_pattern(3, 0x44);

    // Write in non-sequential order to test independence
    device.write_block(2, p2.data(), p2.size());
    device.write_block(0, p0.data(), p0.size());
    device.write_block(3, p3.data(), p3.size());
    device.write_block(1, p1.data(), p1.size());

    // Verify all blocks independently retain their distinct patterns
    std::vector<uint8_t> read_buf(BlockDevice::BLOCK_SIZE, 0x00);

    device.read_block(0, read_buf.data(), read_buf.size());
    ASSERT_EQ(std::memcmp(read_buf.data(), p0.data(), BlockDevice::BLOCK_SIZE), 0);

    device.read_block(1, read_buf.data(), read_buf.size());
    ASSERT_EQ(std::memcmp(read_buf.data(), p1.data(), BlockDevice::BLOCK_SIZE), 0);

    device.read_block(2, read_buf.data(), read_buf.size());
    ASSERT_EQ(std::memcmp(read_buf.data(), p2.data(), BlockDevice::BLOCK_SIZE), 0);

    device.read_block(3, read_buf.data(), read_buf.size());
    ASSERT_EQ(std::memcmp(read_buf.data(), p3.data(), BlockDevice::BLOCK_SIZE), 0);

    // Overwrite block 1 and verify blocks 0, 2, 3 remain intact
    const auto p1_new = make_block_pattern(1, 0x99);
    device.write_block(1, p1_new.data(), p1_new.size());

    device.read_block(1, read_buf.data(), read_buf.size());
    ASSERT_EQ(std::memcmp(read_buf.data(), p1_new.data(), BlockDevice::BLOCK_SIZE), 0);

    device.read_block(0, read_buf.data(), read_buf.size());
    ASSERT_EQ(std::memcmp(read_buf.data(), p0.data(), BlockDevice::BLOCK_SIZE), 0);

    device.read_block(2, read_buf.data(), read_buf.size());
    ASSERT_EQ(std::memcmp(read_buf.data(), p2.data(), BlockDevice::BLOCK_SIZE), 0);

    device.read_block(3, read_buf.data(), read_buf.size());
    ASSERT_EQ(std::memcmp(read_buf.data(), p3.data(), BlockDevice::BLOCK_SIZE), 0);
}

// 5. Preserving data after closing and reopening the image.
void test_preserve_data_after_close_and_reopen() {
    // Note: Reopening verifies data persistence across file handle closure
    // within the file system; this is not a proof of crash durability or physical disk persistence.
    ScopedTestFile file("build/test_reopen.img");
    file.create(BlockDevice::BLOCK_SIZE * 3, 0x00);

    const auto p0 = make_block_pattern(0, 0xA1);
    const auto p2 = make_block_pattern(2, 0xB2);

    {
        BlockDevice device(file.path());
        device.write_block(0, p0.data(), p0.size());
        device.write_block(2, p2.data(), p2.size());
        device.close();
    }

    {
        BlockDevice reopened(file.path());
        ASSERT_TRUE(reopened.is_open());
        ASSERT_EQ(reopened.total_blocks(), static_cast<std::uint64_t>(3));

        std::vector<uint8_t> read_buf(BlockDevice::BLOCK_SIZE, 0x00);
        reopened.read_block(0, read_buf.data(), read_buf.size());
        ASSERT_EQ(std::memcmp(read_buf.data(), p0.data(), BlockDevice::BLOCK_SIZE), 0);

        reopened.read_block(2, read_buf.data(), read_buf.size());
        ASSERT_EQ(std::memcmp(read_buf.data(), p2.data(), BlockDevice::BLOCK_SIZE), 0);
    }
}

// 6. Rejecting an out-of-range block ID.
void test_reject_out_of_range_block_id() {
    ScopedTestFile file("build/test_out_of_range.img");
    file.create(BlockDevice::BLOCK_SIZE * 3, 0x00);

    BlockDevice device(file.path());
    ASSERT_EQ(device.total_blocks(), static_cast<std::uint64_t>(3));

    std::vector<uint8_t> buf(BlockDevice::BLOCK_SIZE, 0x55);

    // Boundary at total_blocks (3): valid block IDs are 0, 1, 2
    ASSERT_THROWS(device.read_block(3, buf.data(), buf.size()), std::out_of_range);
    ASSERT_THROWS(device.write_block(3, buf.data(), buf.size()), std::out_of_range);

    // Far out of range
    ASSERT_THROWS(device.read_block(100, buf.data(), buf.size()), std::out_of_range);
    ASSERT_THROWS(device.write_block(100, buf.data(), buf.size()), std::out_of_range);

    // Maximum uint64 value
    constexpr std::uint64_t max_id = std::numeric_limits<std::uint64_t>::max();
    ASSERT_THROWS(device.read_block(max_id, buf.data(), buf.size()), std::out_of_range);
    ASSERT_THROWS(device.write_block(max_id, buf.data(), buf.size()), std::out_of_range);
}

// 7. Rejecting null read and write buffers.
void test_reject_null_buffers() {
    ScopedTestFile file("build/test_null_buffers.img");
    file.create(BlockDevice::BLOCK_SIZE * 2, 0x00);

    BlockDevice device(file.path());
    ASSERT_THROWS(device.read_block(0, nullptr, BlockDevice::BLOCK_SIZE), std::invalid_argument);
    ASSERT_THROWS(device.write_block(0, nullptr, BlockDevice::BLOCK_SIZE), std::invalid_argument);
}

// 8. Rejecting undersized read and write buffers.
void test_reject_undersized_buffers() {
    ScopedTestFile file("build/test_undersized_buffers.img");
    file.create(BlockDevice::BLOCK_SIZE * 2, 0x00);

    BlockDevice device(file.path());
    std::vector<uint8_t> undersized(BlockDevice::BLOCK_SIZE - 1, 0x00);

    // Size 0
    ASSERT_THROWS(device.read_block(0, undersized.data(), 0), std::invalid_argument);
    ASSERT_THROWS(device.write_block(0, undersized.data(), 0), std::invalid_argument);

    // Size 1
    ASSERT_THROWS(device.read_block(0, undersized.data(), 1), std::invalid_argument);
    ASSERT_THROWS(device.write_block(0, undersized.data(), 1), std::invalid_argument);

    // BLOCK_SIZE - 1 (4095 bytes)
    ASSERT_THROWS(device.read_block(0, undersized.data(), BlockDevice::BLOCK_SIZE - 1), std::invalid_argument);
    ASSERT_THROWS(device.write_block(0, undersized.data(), BlockDevice::BLOCK_SIZE - 1), std::invalid_argument);
}

// 9. Rejecting an empty image file.
void test_reject_empty_image() {
    ScopedTestFile file("build/test_empty.img");
    file.create(0); // 0 bytes

    ASSERT_THROWS(BlockDevice dev(file.path()), std::runtime_error);
}

// 10. Rejecting an image whose size is not a multiple of BLOCK_SIZE.
void test_reject_non_multiple_size_image() {
    // 1 byte
    {
        ScopedTestFile file("build/test_non_mult_1.img");
        file.create(1);
        ASSERT_THROWS(BlockDevice dev(file.path()), std::runtime_error);
    }
    // BLOCK_SIZE - 1 (4095 bytes)
    {
        ScopedTestFile file("build/test_non_mult_4095.img");
        file.create(BlockDevice::BLOCK_SIZE - 1);
        ASSERT_THROWS(BlockDevice dev(file.path()), std::runtime_error);
    }
    // BLOCK_SIZE + 1 (4097 bytes)
    {
        ScopedTestFile file("build/test_non_mult_4097.img");
        file.create(BlockDevice::BLOCK_SIZE + 1);
        ASSERT_THROWS(BlockDevice dev(file.path()), std::runtime_error);
    }
    // Arbitrary non-multiple (5000 bytes)
    {
        ScopedTestFile file("build/test_non_mult_5000.img");
        file.create(5000);
        ASSERT_THROWS(BlockDevice dev(file.path()), std::runtime_error);
    }
}

// 11. Rejecting operations after explicitly closing the device.
void test_reject_operations_after_close() {
    ScopedTestFile file("build/test_closed_ops.img");
    file.create(BlockDevice::BLOCK_SIZE * 2, 0x00);

    BlockDevice device(file.path());
    ASSERT_TRUE(device.is_open());
    device.close();

    ASSERT_FALSE(device.is_open());
    ASSERT_EQ(device.fd(), -1);

    std::vector<uint8_t> buf(BlockDevice::BLOCK_SIZE, 0x00);
    ASSERT_THROWS(device.read_block(0, buf.data(), buf.size()), std::runtime_error);
    ASSERT_THROWS(device.write_block(0, buf.data(), buf.size()), std::runtime_error);

    // Redundant close calls must be safe and idempotent
    device.close();
    ASSERT_FALSE(device.is_open());
}

// 12. Verifying is_open(), size_bytes(), and total_blocks().
void test_inspect_device_properties() {
    ScopedTestFile file("build/test_properties.img");
    constexpr std::size_t num_blocks = 5;
    file.create(BlockDevice::BLOCK_SIZE * num_blocks, 0x00);

    BlockDevice device(file.path());
    ASSERT_TRUE(device.is_open());
    ASSERT_EQ(device.total_blocks(), static_cast<std::uint64_t>(num_blocks));
    ASSERT_EQ(device.size_bytes(), BlockDevice::BLOCK_SIZE * num_blocks);
    ASSERT_TRUE(device.fd() >= 0);

    device.close();
    ASSERT_FALSE(device.is_open());
    ASSERT_EQ(device.fd(), -1);
}

// 13. Testing move construction and move assignment.
void test_move_construction_and_assignment() {
    ScopedTestFile fileA("build/test_move_a.img");
    fileA.create(BlockDevice::BLOCK_SIZE * 2, 0x00);

    ScopedTestFile fileB("build/test_move_b.img");
    fileB.create(BlockDevice::BLOCK_SIZE * 4, 0x00);

    const auto patternA = make_block_pattern(0, 0x11);
    const auto patternB = make_block_pattern(0, 0x22);

    // 13a. Move constructor
    {
        BlockDevice dev1(fileA.path());
        dev1.write_block(0, patternA.data(), patternA.size());
        const int fd1 = dev1.fd();

        BlockDevice dev2(std::move(dev1));
        ASSERT_TRUE(dev2.is_open());
        ASSERT_EQ(dev2.fd(), fd1);
        ASSERT_EQ(dev2.total_blocks(), static_cast<std::uint64_t>(2));
        ASSERT_EQ(dev2.size_bytes(), BlockDevice::BLOCK_SIZE * 2);

        // Moved-from device state must be invalid/closed
        ASSERT_FALSE(dev1.is_open());
        ASSERT_EQ(dev1.fd(), -1);
        ASSERT_EQ(dev1.total_blocks(), static_cast<std::uint64_t>(0));
        ASSERT_EQ(dev1.size_bytes(), static_cast<std::size_t>(0));

        std::vector<uint8_t> buf(BlockDevice::BLOCK_SIZE, 0x00);
        ASSERT_THROWS(dev1.read_block(0, buf.data(), buf.size()), std::runtime_error);
        ASSERT_THROWS(dev1.write_block(0, buf.data(), buf.size()), std::runtime_error);

        // Target device operates properly
        dev2.read_block(0, buf.data(), buf.size());
        ASSERT_EQ(std::memcmp(buf.data(), patternA.data(), BlockDevice::BLOCK_SIZE), 0);
    }

    // 13b. Move assignment
    {
        BlockDevice devA(fileA.path());
        BlockDevice devB(fileB.path());
        devB.write_block(0, patternB.data(), patternB.size());
        const int fdB = devB.fd();

        devA = std::move(devB);
        ASSERT_TRUE(devA.is_open());
        ASSERT_EQ(devA.fd(), fdB);
        ASSERT_EQ(devA.total_blocks(), static_cast<std::uint64_t>(4));
        ASSERT_EQ(devA.size_bytes(), BlockDevice::BLOCK_SIZE * 4);

        ASSERT_FALSE(devB.is_open());
        ASSERT_EQ(devB.fd(), -1);
        ASSERT_EQ(devB.total_blocks(), static_cast<std::uint64_t>(0));
        ASSERT_EQ(devB.size_bytes(), static_cast<std::size_t>(0));

        std::vector<uint8_t> buf(BlockDevice::BLOCK_SIZE, 0x00);
        devA.read_block(0, buf.data(), buf.size());
        ASSERT_EQ(std::memcmp(buf.data(), patternB.data(), BlockDevice::BLOCK_SIZE), 0);

        // 13c. Self-move assignment
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
        devA = std::move(devA);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        ASSERT_TRUE(devA.is_open());
        ASSERT_EQ(devA.fd(), fdB);
        buf.assign(BlockDevice::BLOCK_SIZE, 0x00);
        devA.read_block(0, buf.data(), buf.size());
        ASSERT_EQ(std::memcmp(buf.data(), patternB.data(), BlockDevice::BLOCK_SIZE), 0);
    }
}

// 14. Additional edge cases supported by the implementation
// 14a. Reject opening non-existent file
void test_edge_case_nonexistent_file() {
    ASSERT_THROWS(BlockDevice dev("build/nonexistent_dummy_file_12345.img"), std::system_error);
}

// 14b. Reject opening a directory as backing storage
void test_edge_case_directory_target() {
    ASSERT_THROWS(BlockDevice dev("build"), std::exception);
}

// 14c. Boundary operations on the last block (total_blocks - 1)
void test_edge_case_boundary_last_block() {
    ScopedTestFile file("build/test_boundary_last_block.img");
    constexpr std::uint64_t num_blocks = 4;
    file.create(BlockDevice::BLOCK_SIZE * num_blocks, 0x00);

    BlockDevice device(file.path());
    const std::uint64_t last_block_id = num_blocks - 1;
    const auto pattern = make_block_pattern(last_block_id, 0x77);

    device.write_block(last_block_id, pattern.data(), pattern.size());

    std::vector<uint8_t> read_buf(BlockDevice::BLOCK_SIZE, 0x00);
    device.read_block(last_block_id, read_buf.data(), read_buf.size());

    ASSERT_EQ(std::memcmp(read_buf.data(), pattern.data(), BlockDevice::BLOCK_SIZE), 0);
}

// 14d. Support for buffer_size > BLOCK_SIZE
void test_edge_case_oversized_buffer() {
    ScopedTestFile file("build/test_oversized_buffer.img");
    file.create(BlockDevice::BLOCK_SIZE * 2, 0x00);

    BlockDevice device(file.path());
    const auto pattern = make_block_pattern(0, 0x4B);

    // Oversized buffer of 8192 bytes
    std::vector<uint8_t> write_buf(BlockDevice::BLOCK_SIZE * 2, 0xEE);
    std::memcpy(write_buf.data(), pattern.data(), BlockDevice::BLOCK_SIZE);

    // write_block with buffer_size > BLOCK_SIZE should write exactly BLOCK_SIZE bytes
    device.write_block(0, write_buf.data(), write_buf.size());

    // read_block into oversized buffer initialized with sentinel 0x55
    std::vector<uint8_t> read_buf(BlockDevice::BLOCK_SIZE * 2, 0x55);
    device.read_block(0, read_buf.data(), read_buf.size());

    // First BLOCK_SIZE bytes must match pattern
    ASSERT_EQ(std::memcmp(read_buf.data(), pattern.data(), BlockDevice::BLOCK_SIZE), 0);

    // Bytes beyond BLOCK_SIZE must remain completely untouched
    for (std::size_t i = BlockDevice::BLOCK_SIZE; i < read_buf.size(); ++i) {
        ASSERT_EQ(read_buf[i], static_cast<uint8_t>(0x55));
    }
}

// 14e. Idempotent close
void test_edge_case_idempotent_close() {
    ScopedTestFile file("build/test_idempotent_close.img");
    file.create(BlockDevice::BLOCK_SIZE, 0x00);

    BlockDevice device(file.path());
    ASSERT_TRUE(device.is_open());

    device.close();
    ASSERT_FALSE(device.is_open());
    ASSERT_EQ(device.fd(), -1);

    // Multiple consecutive close calls must not throw or crash
    device.close();
    device.close();
    ASSERT_FALSE(device.is_open());
    ASSERT_EQ(device.fd(), -1);
}

// 14f. Destructor closes underlying file descriptor
void test_edge_case_destructor_closes_fd() {
    ScopedTestFile file("build/test_destructor_closes_fd.img");
    file.create(BlockDevice::BLOCK_SIZE, 0x00);

    int saved_fd = -1;
    {
        BlockDevice device(file.path());
        saved_fd = device.fd();
        ASSERT_TRUE(saved_fd >= 0);
        // Verify fd is currently valid
        ASSERT_TRUE(::fcntl(saved_fd, F_GETFD) != -1);
    }

    // Now out of scope: destructor should have closed saved_fd
    errno = 0;
    const int ret = ::fcntl(saved_fd, F_GETFD);
    ASSERT_EQ(ret, -1);
    ASSERT_EQ(errno, EBADF);
}

// ============================================================================
// Test Suite Runner
// ============================================================================

struct TestRunner {
    int total = 0;
    int passed = 0;
    int failed = 0;

    void run(const std::string& name, const std::function<void()>& fn) {
        ++total;
        try {
            fn();
            ++passed;
            std::cout << "  [PASS] " << name << "\n";
        } catch (const TestFailure& tf) {
            ++failed;
            std::cerr << "  [FAIL] " << name << "\n"
                      << "         " << tf.what() << "\n";
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
    std::cout << "              Filler Storage: BlockDevice Test Suite                 \n";
    std::cout << "======================================================================\n";

    TestRunner runner;

    // Core requirements
    runner.run("1. Opening a valid, non-empty image file", test_open_valid_image);
    runner.run("2. Correct image size and total block count", test_image_size_and_total_blocks);
    runner.run("3. Writing a complete block and reading it back", test_write_and_read_single_block);
    runner.run("4. Reading and writing multiple different blocks independently", test_read_write_multiple_blocks_independently);
    runner.run("5. Preserving data across close and reopen", test_preserve_data_after_close_and_reopen);
    runner.run("6. Rejecting out-of-range block IDs", test_reject_out_of_range_block_id);
    runner.run("7. Rejecting null read and write buffers", test_reject_null_buffers);
    runner.run("8. Rejecting undersized read and write buffers", test_reject_undersized_buffers);
    runner.run("9. Rejecting an empty image file", test_reject_empty_image);
    runner.run("10. Rejecting images with non-multiple block sizes", test_reject_non_multiple_size_image);
    runner.run("11. Rejecting operations after explicitly closing device", test_reject_operations_after_close);
    runner.run("12. Verifying is_open(), size_bytes(), and total_blocks()", test_inspect_device_properties);
    runner.run("13. Move construction and move assignment semantics", test_move_construction_and_assignment);

    // Edge cases
    runner.run("14a. Rejecting non-existent image file", test_edge_case_nonexistent_file);
    runner.run("14b. Rejecting directory as image storage", test_edge_case_directory_target);
    runner.run("14c. Boundary block operations at total_blocks - 1", test_edge_case_boundary_last_block);
    runner.run("14d. Handling buffer_size greater than BLOCK_SIZE", test_edge_case_oversized_buffer);
    runner.run("14e. Idempotent close calls", test_edge_case_idempotent_close);
    runner.run("14f. Destructor releases underlying file descriptor", test_edge_case_destructor_closes_fd);

    std::cout << "======================================================================\n";
    std::cout << "Test Summary: " << runner.passed << " / " << runner.total
              << " passed (" << runner.failed << " failed)\n";
    std::cout << "======================================================================\n";

    return (runner.failed == 0) ? 0 : 1;
}
