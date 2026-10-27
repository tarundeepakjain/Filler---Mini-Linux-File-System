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

# Test configuration
TEST_BIN := $(BIN_DIR)/block_device_test

.PHONY: all clean dirs test

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

# Build and run test suite
test: dirs
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
		src/storage/BlockDevice.cpp \
		tests/block_device_test.cpp \
		-o $(TEST_BIN)
	./$(TEST_BIN)
