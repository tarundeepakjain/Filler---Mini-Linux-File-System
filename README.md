# Filler – Custom Userspace Mini File System

[![Language: C++17](https://img.shields.io/badge/Language-C%2B%2B17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B17)
[![Build & Tests](https://img.shields.io/badge/Tests-113%2F113%20Passed-brightgreen.svg)](docs/TESTING.md)
[![License: Educational](https://img.shields.io/badge/License-Educational-orange.svg)](#)

**Filler** is a fully functional, highly reliable userspace file system written in ISO C++17. It manages a virtual 64 MiB binary disk image using classic Unix filesystem principles: a centralized Superblock, dedicated block and inode allocation bitmaps, a fixed-size Inode Table, hierarchical directory indexing, and multi-level indirect block addressing.

Filler runs entirely in user space and features its own interactive UNIX-like shell REPL, enabling deep study of operating system storage algorithms without the risk of host kernel crashes.

---

## Table of Contents

- [1. Project Overview](#1-project-overview)
- [2. Verified Features](#2-verified-features)
- [3. Technology Stack](#3-technology-stack)
- [4. High-Level Architecture](#4-high-level-architecture)
- [5. Filesystem Configuration](#5-filesystem-configuration)
- [6. Project Directory Structure](#6-project-directory-structure)
- [7. Build, Test & Run Guide](#7-build-test--run-guide)
- [8. Example Shell Session](#8-example-shell-session)
- [9. Technical Documentation Map](#9-technical-documentation-map)

---

## 1. Project Overview

### What is a Filesystem?
At the physical hardware level, secondary storage devices (SSDs, hard disks, USB drives) present storage as a flat, unorganized array of fixed-size sectors. A **filesystem** is the foundational operating system subsystem that provides structure, persistence, and abstraction over this raw storage. It introduces named files, hierarchical directory trees, metadata attributes (sizes, permissions, timestamps), and space allocation tracking.

### What is a Userspace Filesystem?
Standard production filesystems (such as Linux ext4, Btrfs, or XFS) run inside the OS kernel (Ring 0). A bug in kernel filesystem code causes a kernel panic and crashes the entire machine. A **userspace filesystem** executes as a standard user process (Ring 3). Filler interacts with an underlying 64 MiB regular file (`filler.img`) via standard POSIX system calls (`open`, `read`, `write`, `lseek`, `ftruncate`), treating that file as a virtual physical block device.

### Difference Between Filler and Conventional Linux Filesystems
| Characteristic | Conventional Linux FS (ext4) | Filler Userspace FS |
|---|---|---|
| **Execution Domain** | Kernel Space (Ring 0 VFS) | User Space (Ring 3 Process) |
| **Mount Mechanism** | Native `mount /dev/sda1 /mnt` | Custom interactive Shell REPL |
| **Crash Blast Radius** | Kernel Panic (reboot required) | Process crash (host completely safe) |
| **Crash Protection** | Write-Ahead Journaling (JBD2) | Truncate rollback & ordered unlinking |
| **Addressing** | Extent trees (`struct ext4_extent`) | 16 Direct + Single + Double Indirect |
| **Debugging** | Kernel debuggers (`kgdb`, JTAG) | Standard userspace debuggers (`gdb`, Valgrind) |

### Core Operating Systems Concepts Demonstrated
1. **Block Storage Abstraction**: Translating high-level stream I/O into 4096-byte block read/write operations.
2. **Metadata Serialization**: Little-endian binary serialization guaranteed across CPU architectures.
3. **Space Management**: First-fit allocation and double-free protection using compact bitmaps.
4. **Hierarchical Namespaces**: Directory tree traversal, path tokenization, and packed directory entries.
5. **Multi-Level Indexing**: Direct pointers, single-indirect tables, and double-indirect pointer cascades.
6. **Failure Recovery**: In-memory allocation rollback and ordered deletion to prevent dangling directory references.

---

## 2. Verified Features

Every feature listed below is verified in the C++17 source code and passes automated testing:
- **Interactive Shell REPL**: Integrated shell with prompt tracking current directory (`filler:/docs> `).
- **Directory Hierarchy**: Support for nested directories, `.` (self) and `..` (parent) entries, and absolute/relative path resolution.
- **Multi-Level Block Addressing**: 16 direct block pointers (64 KiB), 1 single-indirect pointer (2 MiB), and 1 double-indirect pointer (1 GiB theoretical capacity).
- **Safe Truncation & Rollback**: Automatic allocation tracking with in-memory rollback that restores modified indirect tables and releases newly allocated blocks if allocation fails midway.
- **Fail-Safe Deletion Ordering**: Unlinks directory entries *before* deallocating inode and data blocks, preventing dangling directory references.
- **Volume Inspection**: Automatic volume classification into `VALID`, `UNFORMATTED`, `CORRUPTED`, `INVALID_IMAGE`, or `IO_ERROR`.
- **Automatic Image Formatting**: Formats blank or undersized images into 64 MiB volumes upon user command.
- **POSIX-Style Shell Commands**: Supports `format`, `pwd`, `cd`, `ls`, `mkdir`, `touch`, `write`, `append`, `cat`, `truncate`, `rm`, `rmdir`, `stat`, `help`, and `exit`.

---

## 3. Technology Stack

- **Language**: C++17 (ISO/IEC 14882:2017)
- **Compiler Flags**: `-std=c++17 -Wall -Wextra -Wpedantic -Werror -O2 -g`
- **Host OS Support**: Linux (Ubuntu, Debian, Fedora, Arch) and macOS (Intel & Apple Silicon)
- **System Call APIs**: POSIX.1-2008 (`open`, `close`, `lseek`, `read`, `write`, `fstat`, `ftruncate`)
- **Build System**: GNU Make with automatic dependency tracking (`-MMD -MP`)
- **Testing Framework**: Custom zero-dependency C++ test runner with failure injection hooks

---

## 4. High-Level Architecture

```mermaid
graph TD
    User([User Terminal]) --> Shell[Shell REPL / CLI]
    
    subgraph "Filesystem Subsystem Layer"
        Shell --> FM[FileManager: File R/W, Truncate Rollback]
        Shell --> DM[DirectoryManager: Namespaces, Lookup, Mkdir]
        Shell --> Formatter[Formatter: Disk Initialization]
        
        FM --> DM
        FM --> IM[InodeManager: Inode Lifecycle & Table R/W]
        FM --> BM[BlockBitmap: First-Fit Alloc & Free]
        DM --> IM
        DM --> BM
        Formatter --> BM
        Formatter --> IM
    end
    
    subgraph "Metadata Layer"
        IM --> Inode[Inode: 256-byte Little-Endian Records]
        BM --> SB[Superblock: Block 0 Geometry & Layout]
        IM --> SB
        Formatter --> SB
    end
    
    subgraph "Storage Layer"
        FM --> BD[BlockDevice Abstraction: 4096-byte Bounds-Checked I/O]
        DM --> BD
        IM --> BD
        BM --> BD
        Formatter --> BD
        SB --> BD
    end
    
    subgraph "Host OS"
        BD --> POSIX[POSIX System Calls: pread/pwrite/lseek]
        POSIX --> DiskImg[(Disk Image: filler.img - 64 MiB)]
    end
```

---

## 5. Filesystem Configuration

Filler enforces strict physical configuration parameters across all components:

| Parameter | Configured Value | Value in Hex / Bytes | Design Rationale |
|---|---|---|---|
| **Disk Image Size** | **64 MiB** | 67,108,864 bytes (`0x04000000`) | Sufficient to demonstrate multi-level indirect blocks; compact enough for instant test execution. |
| **Block Size** | **4096 bytes** | 4,096 bytes (`0x1000`) | Matches host OS page size (x86/ARM); allows exactly 512 64-bit pointers per indirect block. |
| **Total Blocks** | **16,384 blocks** | `0x4000` | \(67,108,864 / 4096 = 16,384\). Fits cleanly in half of a 4096-byte bitmap block. |
| **Inode Size** | **256 bytes** | `0x0100` | Exact power of 2; exactly 16 inodes fit into each 4096-byte inode table block without slack. |
| **Total Inodes** | **256 inodes** | `0x0100` | Balances files against storage size; requires only 32 bytes in the inode bitmap. |
| **Root Inode** | **Inode 1** | `0x00000001` | First valid allocatable inode after reserving Inode 0. Assigned to `/`. |
| **Reserved Inode** | **Inode 0** | `0x00000000` | Sentinel value to prevent uninitialized or zeroed references from pointing to valid files. |
| **Magic Number** | **`0x46494C4C`** | ASCII `"FILL"` | Identifies the disk image as a valid Filler filesystem. |
| **Filesystem Version** | **1** | `0x00000001` | Protects against incompatible on-disk layout modifications. |
| **Serialization** | **Little-Endian** | Explicit bitwise shifts | Ensures binary cross-platform portability between Intel x86 and ARM64. |

---

## 6. Project Directory Structure

```text
Filler/
├── Makefile                        # Build targets for bin/filler and all 9 test suites
├── include/                        # Public C++ header files
│   ├── cli/
│   │   └── Shell.hpp               # Interactive REPL, command dispatch, path resolver
│   ├── fs/
│   │   ├── BlockBitmap.hpp         # Free block bitmap allocation and exhaustion tracking
│   │   ├── DirectoryManager.hpp    # Directory records, lookup, mkdir, rmdir
│   │   ├── FileManager.hpp         # File R/W, sparse gaps, truncate rollback context
│   │   ├── Formatter.hpp           # Disk initialization and region formatting
│   │   ├── Inode.hpp               # 256-byte Inode structure and serialization offsets
│   │   ├── InodeManager.hpp        # Inode table R/W, allocation, boundary isolation
│   │   └── Superblock.hpp          # Superblock header, offsets, and invariant validator
│   └── storage/
│       └── BlockDevice.hpp         # 4096-byte positional block device abstraction
├── src/                            # C++ implementation files
│   ├── main.cpp                    # Binary entry point
│   ├── cli/
│   │   └── Shell.cpp               # REPL loop, command handlers, volume classifier
│   ├── fs/
│   │   ├── BlockBitmap.cpp         # Bitmap allocation algorithms
│   │   ├── DirectoryManager.cpp    # Packed directory record serializer
│   │   ├── FileManager.cpp         # File operations and indirect block traversal
│   │   ├── Formatter.cpp           # Formatting routines
│   │   ├── Inode.cpp               # Inode endianness-safe serializer
│   │   ├── InodeManager.cpp        # Inode table read-modify-write engine
│   │   └── Superblock.cpp          # Superblock serialization and validation logic
│   └── storage/
│       └── BlockDevice.cpp         # POSIX file descriptor wrapper with lseek/read/write
├── tests/                          # Standalone test suites (113 tests total)
│   ├── block_device_test.cpp       # BlockDevice bounds and I/O tests (19 tests)
│   ├── superblock_test.cpp         # Superblock round-trip and validation tests (14 tests)
│   ├── formatter_test.cpp          # Formatter region initialization tests (8 tests)
│   ├── block_bitmap_test.cpp       # Bitmap allocation and exhaustion tests (13 tests)
│   ├── inode_manager_test.cpp      # Inode table and isolation tests (12 tests)
│   ├── directory_manager_test.cpp  # Directory hierarchy and lookup tests (15 tests)
│   ├── file_manager_test.cpp       # Multi-block, indirect, rollback tests (14 tests)
│   ├── cli_test.cpp                # Shell REPL and state classification tests (11 tests)
│   └── integration_test.cpp        # Full end-to-end integration scenarios (7 tests)
└── docs/                           # Comprehensive technical documentation
    ├── ARCHITECTURE.md             # In-depth subsystem architectural guide
    ├── DATA_LAYOUT.md              # Exact on-disk block and byte map
    ├── DESIGN_DECISIONS.md         # Detailed design decision and trade-off log
    ├── INTERNALS.md                # Step-by-step algorithms and pseudocode
    ├── LIMITATIONS.md              # Honest technical constraints and future roadmap
    ├── OPERATIONS.md               # Complete CLI command reference and lifecycle trace
    ├── TESTING.md                  # Testing architecture and failure-injection guide
    └── VIVA_QUESTIONS.md           # 55 OS viva and technical interview questions
```

---

## 7. Build, Test & Run Guide

### Prerequisites
- GCC / G++ (supporting C++17) or Clang++
- GNU Make
- Linux or macOS operating system

### 1. Compile the Project
```bash
make clean && make all
```
This builds the binary at `bin/filler`.

### 2. Run All Test Suites
```bash
make test
```
Runs all 9 test suites (113 tests). Every test must pass with 0 failures.

### 3. Launch the Interactive Shell
```bash
# Launch with default disk image (filler.img)
make run

# Alternatively, specify a custom disk image:
./bin/filler my_disk.img
```

---

## 8. Example Shell Session

Below is a realistic terminal session demonstrating actual supported commands:

```text
$ make run
./bin/filler
Filler Mini File System Shell (image: filler.img)
Type 'help' for available commands or 'exit' to quit.

filler:/> format
Filesystem successfully formatted.

filler:/> ls
[DIR]  .
[DIR]  ..

filler:/> mkdir docs
filler:/> cd docs
filler:/docs> touch notes.txt
filler:/docs> write notes.txt "Welcome to Filler File System!\n"
filler:/docs> append notes.txt "Built with C++17 and multi-level indirect blocks.\n"

filler:/docs> cat notes.txt
Welcome to Filler File System!
Built with C++17 and multi-level indirect blocks.

filler:/docs> stat notes.txt
Inode:       3
Type:        Regular File
Permissions: 0644
UID/GID:     0/0
Size:        80 bytes
Blocks:      1
Access Time: 2026-11-14 13:45:10
Modify Time: 2026-11-14 13:45:10
Change Time: 2026-11-14 13:45:10
Direct Blocks: [ 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 ]
Single Indirect: 0
Double Indirect: 0

filler:/docs> cd ..
filler:/> ls docs
[FILE] notes.txt (80 bytes)

filler:/> exit
```

---

## 9. Technical Documentation Map

For deep technical insights, consult the dedicated guides under [`docs/`](docs/):

- [**docs/ARCHITECTURE.md**](docs/ARCHITECTURE.md): Complete subsystem design, layering, positional I/O, and component interactions.
- [**docs/DATA_LAYOUT.md**](docs/DATA_LAYOUT.md): Exact physical disk map, region boundaries, mathematical mapping formulas, and endianness.
- [**docs/DESIGN_DECISIONS.md**](docs/DESIGN_DECISIONS.md): The "Why" behind every design choice, trade-offs, and alternative analysis.
- [**docs/INTERNALS.md**](docs/INTERNALS.md): Step-by-step algorithms, pseudocode, and time complexities for all 16 core operations.
- [**docs/OPERATIONS.md**](docs/OPERATIONS.md): Comprehensive CLI command guide, file lifecycle traces, and persistence behavior.
- [**docs/TESTING.md**](docs/TESTING.md): Detailed verification suite guide, failure-injection mechanics, and test counts.
- [**docs/LIMITATIONS.md**](docs/LIMITATIONS.md): Honest assessment of missing features (no journaling, no FUSE) and engineering roadmap.
- [**docs/VIVA_QUESTIONS.md**](docs/VIVA_QUESTIONS.md): 55 comprehensive questions and answers for viva or technical interview preparation.

