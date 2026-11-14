# Compiler and flags
CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -Werror -O2 -g
CPPFLAGS := -Iinclude -MMD -MP

# Directories
SRC_DIR := src
INCLUDE_DIR := include
BUILD_DIR := build
BIN_DIR := bin

# Source files and corresponding object files
SRCS := $(shell find $(SRC_DIR) -name '*.cpp' 2>/dev/null)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Test binaries
TEST_BLOCK_DEVICE_BIN := $(BIN_DIR)/block_device_test
TEST_SUPERBLOCK_BIN := $(BIN_DIR)/superblock_test
TEST_FORMATTER_BIN := $(BIN_DIR)/formatter_test
TEST_BLOCK_BITMAP_BIN := $(BIN_DIR)/block_bitmap_test
TEST_INODE_MANAGER_BIN := $(BIN_DIR)/inode_manager_test
TEST_DIRECTORY_MANAGER_BIN := $(BIN_DIR)/directory_manager_test
TEST_FILE_MANAGER_BIN := $(BIN_DIR)/file_manager_test
TEST_CLI_BIN := $(BIN_DIR)/cli_test
TEST_INTEGRATION_BIN := $(BIN_DIR)/integration_test

.PHONY: all clean dirs run test test_block_device test_superblock test_formatter test_block_bitmap test_inode_manager test_directory_manager test_file_manager test_cli test_integration

# Default target
all: dirs $(BIN_DIR)/filler

# Create build directories
dirs:
	mkdir -p $(BUILD_DIR) $(BIN_DIR)

# Link object files into executable
$(BIN_DIR)/filler: $(OBJS) | dirs
	$(CXX) $(CXXFLAGS) $^ -o $@

# Compile source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# Include generated dependency files
-include $(DEPS)

# Remove generated files
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# Build and run block device test suite
test_block_device: dirs
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
		src/storage/BlockDevice.cpp \
		tests/block_device_test.cpp \
		-o $(TEST_BLOCK_DEVICE_BIN)
	./$(TEST_BLOCK_DEVICE_BIN)

# Build and run superblock test suite
test_superblock: dirs
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
		src/storage/BlockDevice.cpp \
		src/fs/Superblock.cpp \
		tests/superblock_test.cpp \
		-o $(TEST_SUPERBLOCK_BIN)
	./$(TEST_SUPERBLOCK_BIN)

# Build and run formatter test suite
test_formatter: dirs
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
		src/storage/BlockDevice.cpp \
		src/fs/Superblock.cpp \
		src/fs/Formatter.cpp \
		tests/formatter_test.cpp \
		-o $(TEST_FORMATTER_BIN)
	./$(TEST_FORMATTER_BIN)

# Build and run block bitmap test suite
test_block_bitmap: dirs
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
		src/storage/BlockDevice.cpp \
		src/fs/Superblock.cpp \
		src/fs/Formatter.cpp \
		src/fs/BlockBitmap.cpp \
		tests/block_bitmap_test.cpp \
		-o $(TEST_BLOCK_BITMAP_BIN)
	./$(TEST_BLOCK_BITMAP_BIN)

# Build and run inode manager test suite
test_inode_manager: dirs
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
		src/storage/BlockDevice.cpp \
		src/fs/Superblock.cpp \
		src/fs/Formatter.cpp \
		src/fs/Inode.cpp \
		src/fs/InodeManager.cpp \
		tests/inode_manager_test.cpp \
		-o $(TEST_INODE_MANAGER_BIN)
	./$(TEST_INODE_MANAGER_BIN)

# Build and run directory manager test suite
test_directory_manager: dirs
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
		src/storage/BlockDevice.cpp \
		src/fs/Superblock.cpp \
		src/fs/Formatter.cpp \
		src/fs/BlockBitmap.cpp \
		src/fs/Inode.cpp \
		src/fs/InodeManager.cpp \
		src/fs/DirectoryManager.cpp \
		tests/directory_manager_test.cpp \
		-o $(TEST_DIRECTORY_MANAGER_BIN)
	./$(TEST_DIRECTORY_MANAGER_BIN)

# Build and run file manager test suite
test_file_manager: dirs
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
		src/storage/BlockDevice.cpp \
		src/fs/Superblock.cpp \
		src/fs/Formatter.cpp \
		src/fs/BlockBitmap.cpp \
		src/fs/Inode.cpp \
		src/fs/InodeManager.cpp \
		src/fs/DirectoryManager.cpp \
		src/fs/FileManager.cpp \
		tests/file_manager_test.cpp \
		-o $(TEST_FILE_MANAGER_BIN)
	./$(TEST_FILE_MANAGER_BIN)

# Run interactive shell
run: all
	./$(BIN_DIR)/filler

# Build and run CLI test suite
test_cli: dirs
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
		src/storage/BlockDevice.cpp \
		src/fs/Superblock.cpp \
		src/fs/Formatter.cpp \
		src/fs/BlockBitmap.cpp \
		src/fs/Inode.cpp \
		src/fs/InodeManager.cpp \
		src/fs/DirectoryManager.cpp \
		src/fs/FileManager.cpp \
		src/cli/Shell.cpp \
		tests/cli_test.cpp \
		-o $(TEST_CLI_BIN)
	./$(TEST_CLI_BIN)

# Build and run End-to-End Integration test suite
test_integration: dirs
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
		src/storage/BlockDevice.cpp \
		src/fs/Superblock.cpp \
		src/fs/Formatter.cpp \
		src/fs/BlockBitmap.cpp \
		src/fs/Inode.cpp \
		src/fs/InodeManager.cpp \
		src/fs/DirectoryManager.cpp \
		src/fs/FileManager.cpp \
		src/cli/Shell.cpp \
		tests/integration_test.cpp \
		-o $(TEST_INTEGRATION_BIN)
	./$(TEST_INTEGRATION_BIN)

# Run all test suites
test: test_block_device test_superblock test_formatter test_block_bitmap test_inode_manager test_directory_manager test_file_manager test_cli test_integration
