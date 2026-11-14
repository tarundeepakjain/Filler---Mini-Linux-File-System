# Filler – Testing, Verification & Failure Injection Guide

This document describes the complete testing strategy, test harness architecture, verification suites, failure-injection mechanisms, and edge case coverage for **Filler**. All test suites are built and executed using standard Makefile targets and run cleanly without warnings under `-Wall -Wextra -Wpedantic -Werror`.

---

## 1. Test Suite Summary

Every layer of Filler is independently verified by a dedicated standalone test binary, culminating in comprehensive CLI and End-to-End integration test suites.

| # | Test Suite | Binary Target | Source File | Test Count | Status |
|---|---|---|---|---:|:---:|
| 1 | **BlockDevice Test Suite** | `bin/block_device_test` | `tests/block_device_test.cpp` | 19 | **19 / 19 PASS** |
| 2 | **Superblock Test Suite** | `bin/superblock_test` | `tests/superblock_test.cpp` | 14 | **14 / 14 PASS** |
| 3 | **Formatter Test Suite** | `bin/formatter_test` | `tests/formatter_test.cpp` | 8 | **8 / 8 PASS** |
| 4 | **BlockBitmap Test Suite** | `bin/block_bitmap_test` | `tests/block_bitmap_test.cpp` | 13 | **13 / 13 PASS** |
| 5 | **InodeManager Test Suite** | `bin/inode_manager_test` | `tests/inode_manager_test.cpp` | 12 | **12 / 12 PASS** |
| 6 | **DirectoryManager Test Suite** | `bin/directory_manager_test` | `tests/directory_manager_test.cpp` | 15 | **15 / 15 PASS** |
| 7 | **FileManager Test Suite** | `bin/file_manager_test` | `tests/file_manager_test.cpp` | 14 | **14 / 14 PASS** |
| 8 | **Shell CLI Test Suite** | `bin/cli_test` | `tests/cli_test.cpp` | 11 | **11 / 11 PASS** |
| 9 | **End-to-End Integration Suite**| `bin/integration_test` | `tests/integration_test.cpp` | 7 | **7 / 7 PASS** |
| **Total** | | | | **113** | **113 / 113 PASS (0 FAIL)** |

---

## 2. Test Execution Commands

### Run All 9 Test Suites in Sequence
```bash
make test
```

### Run Individual Test Suites
```bash
make test_block_device        # Storage abstraction tests
make test_superblock          # Superblock serialization & validation
make test_formatter           # Filesystem formatting initialization
make test_block_bitmap        # Block bitmap allocation & exhaustion
make test_inode_manager       # Inode lifecycle & boundary isolation
make test_directory_manager   # Directory hierarchy & record packing
make test_file_manager        # File R/W, multi-block, truncate rollback
make test_cli                 # Shell CLI REPL & state classification
make test_integration         # Full E2E Scenarios A through F
```

---

## 3. Detailed Test Suite Coverage

### 3.1 BlockDevice Test Suite (`tests/block_device_test.cpp`)
- **Focus**: Verifies raw 4096-byte block read and write semantics against the host OS file descriptor.
- **Key Tests**:
  - Opening valid non-empty files; calculating exact size and total block count.
  - Writing a complete block and reading it back bit-for-bit.
  - Multi-block independent read/write isolation.
  - Data preservation across explicit `close()` and reopen.
  - Rejection of out-of-range block IDs (`block_id >= total_blocks`).
  - Rejection of null buffers and undersized read/write buffers (< 4096 bytes).
  - Rejection of empty (0-byte) backing files and non-multiple block sizes.
  - Boundary block operations at `total_blocks - 1` (Block 16,383).
  - Move construction and move assignment semantics.
  - RAII file descriptor cleanup upon destruction.

### 3.2 Superblock Test Suite (`tests/superblock_test.cpp`)
- **Focus**: Verifies 120-byte serialization, deserialization, and structural invariant validation.
- **Key Tests**:
  - Little-endian round-trip preservation for all 17 fields.
  - Rejection of corrupted magic numbers (anything other than `0x46494C4C`).
  - Rejection of unsupported versions (e.g. version 2 or 99).
  - Rejection of invalid block sizes (e.g. 512, 2048, 8192).
  - Rejection of overlapping or disordered regions.
  - Insufficient bitmap or inode-table capacity validation.
  - Prevention of arithmetic overflow in region calculations.
  - Validation that invalid superblocks are rejected without modifying disk content.

### 3.3 Formatter Test Suite (`tests/formatter_test.cpp`)
- **Focus**: Verifies clean initialization of a blank disk image.
- **Key Tests**:
  - Creation of exact Superblock metadata.
  - Block Bitmap initialization: Blocks 0..18 marked allocated (`1`), Blocks 19..16,383 marked free (`0`).
  - Inode Bitmap initialization: Inodes 0 and 1 marked allocated, Inodes 2..255 marked free.
  - Full zero-filling of Inode Table (Blocks 3..18).
  - Root directory creation with `.` and `..` pointing to Inode 1.
  - Rejection of undersized devices when standard size is enforced.

### 3.4 BlockBitmap Test Suite (`tests/block_bitmap_test.cpp`)
- **Focus**: Verifies block allocation, deallocation, exhaustion, and double-free protection.
- **Key Tests**:
  - Sequential first-fit allocation returns Block 19, 20, 21, etc.
  - Deallocation clears bits and restores free block counter.
  - Double-free detection: Attempting to free an already-freed block throws `BlockBitmapError`.
  - Protection of metadata blocks: Attempting to free Blocks 0 through 18 throws `BlockBitmapError`.
  - Full bitmap exhaustion: Allocates all 16,365 blocks, asserts `NoFreeBlocksError`, frees one block, and re-allocates successfully.
  - Persistence of bitmap bits across manager reload.

### 3.5 InodeManager Test Suite (`tests/inode_manager_test.cpp`)
- **Focus**: Verifies 256-byte inode serialization, allocation, update, and boundary isolation.
- **Key Tests**:
  - Exact 256-byte serialization/deserialization with nanosecond timestamps and permissions.
  - Allocation returns Inode 2 first (skipping 0 and 1).
  - Neighboring inode isolation: Updating Inode 4 does not alter Inode 3 or 5 within the same 4096-byte block.
  - Protection of reserved inodes: Attempting to free Inode 0 or Inode 1 throws `InodeManagerError`.
  - Double-free rejection on inodes.
  - Inode exhaustion and recovery across all 256 inodes.

### 3.6 DirectoryManager Test Suite (`tests/directory_manager_test.cpp`)
- **Focus**: Verifies directory hierarchy, packed record alignment, and path entry management.
- **Key Tests**:
  - Root directory contains `.` and `..` pointing to Inode 1.
  - Directory creation (`mkdir`) and listing (`list`).
  - Duplicate filename rejection within the same parent directory.
  - Rejecting `mkdir` inside a regular file.
  - Nested directory creation (`/a/b/c/d`) and traversal.
  - Empty directory removal (`rmdir`) and rejection of non-empty directory removal.
  - Rejection of root directory deletion and `.` / `..` deletion.
  - Persistence across reopening the disk image.

### 3.7 FileManager Test Suite (`tests/file_manager_test.cpp`)
- **Focus**: Verifies file I/O, multi-block allocations, indirect pointer handling, and failure safety.
- **Key Tests**:
  - Writing and reading data across partial blocks and multi-block files.
  - Sparse gap handling: Writing at offset 10,000 in an empty file creates zero-filled gaps.
  - Truncation to smaller sizes (releasing blocks and zeroing slack space).
  - Single-indirect block support: Creating and verifying files larger than 64 KiB (> 16 blocks).
  - Double-indirect block support: Allocating through Level-1 and Level-2 pointer tables.
  - **Allocation Failure & Truncate Rollback (Test 13)**:
    - Fills bitmap to 2 free blocks; truncates by 5 blocks.
    - Verifies exception thrown, zero blocks leaked, indirect tables restored, and original file content preserved.
  - **Safe Deletion Ordering (Test 14)**:
    - Tests failure injection on directory entry unlinking.
    - Verifies file inode and data blocks remain allocated if unlinking fails.

### 3.8 Shell CLI Test Suite (`tests/cli_test.cpp`)
- **Focus**: Verifies command parsing, REPL loops, and volume state classification.
- **Key Tests**:
  - Fresh blank image classification: Classified as `UNFORMATTED`.
  - Format command initializes unformatted and undersized images.
  - Command dispatch: `pwd`, `cd`, `ls`, `mkdir`, `touch`, `write`, `append`, `cat`, `truncate`, `rm`, `rmdir`, `stat`.
  - Image state classification:
    - Erased magic number with existing metadata -> `CORRUPTED`.
    - Wiped Block 0 with bitmap data -> `CORRUPTED`.
    - Nonzero invalid magic (`0xDEADBEEF`) -> `INVALID_IMAGE`.
    - Unsupported version (`99`) -> `INVALID_IMAGE`.
    - Out-of-bounds data region offset -> `CORRUPTED`.
  - Persistence across independent shell instances.

### 3.9 End-to-End Integration Suite (`tests/integration_test.cpp`)
- **Scenario A: Basic Filesystem**: Format disk, verify root directory, verify superblock counters.
- **Scenario B: Directory Operations**: Nested directory hierarchies, relative and absolute traversal, `.` and `..` handling.
- **Scenario C: File Operations**: Multi-block file creation, read-back verification, partial overwrite, append.
- **Scenario D: Persistence**: Create complex tree, close shell, reopen disk image, verify every file, directory, and byte.
- **Scenario E: Resource Management**: Allocate large files, delete them, verify block and inode counts return to baseline.
- **Scenario F: Invalid Operations**: Rejection of path traversal out of bounds, duplicate creation, deleting non-empty directories.
- **Data Integrity & Consistency**: Bit-by-bit comparison of 100 KiB pseudorandom file content written and read back.

---

## 4. Failure-Injection Architecture

To verify robustness under adverse conditions, Filler implements failure-injection tests without modifying production interfaces:

### A. Bitmap Exhaustion Simulation
In `test_truncate_growth_and_rollback`:
1. The test allocates blocks until exactly 2 free blocks remain.
2. It requests a truncation requiring 5 additional blocks.
3. The third block allocation fails with `NoFreeBlocksError`.
4. The test verifies:
   - Free block count is identical before and after.
   - Indirect tables on disk have freed pointers restored to 0.
   - Original file content is completely intact.

### B. Directory Unlinking Failure Simulation
In `test_remove_safe_deletion_ordering`:
1. A test subclass `FailingDirectoryManager` overrides `remove_entry()`.
2. A flag `inject_remove_failure` triggers a simulated I/O error during unlinking.
3. The test verifies:
   - Directory entry was not unlinked.
   - Inode was not prematurely freed.
   - File data blocks were not freed.
   - File data remains 100% accessible.

