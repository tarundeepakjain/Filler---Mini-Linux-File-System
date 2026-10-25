# Compiler and flags
CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Werror -O2 -g
CPPFLAGS ?= -Iinclude -MMD -MP

# Directories
SRC_DIR := src
INCLUDE_DIR := include
BUILD_DIR := build
BIN_DIR := bin

# Source files and corresponding object files
SRCS := $(shell find $(SRC_DIR) -name '*.cpp' 2>/dev/null)
OBJS := $(SRCS:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean dirs

# Default target compiles all object files
all: dirs $(OBJS)

# Ensure required directories exist
dirs:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)

# Compile C++ source files into object files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# Include compiler-generated dependency files (*.d)
-include $(DEPS)

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
