# Filler – Design Decisions & Engineering Trade-offs

This document provides a comprehensive log of the major architectural and engineering design decisions made in **Filler**. For each decision, we record the chosen approach, the technical rationale, advantages, trade-offs, evaluated alternatives, why those alternatives were rejected, and whether the choice represents a permanent limitation or a deliberate architectural simplification.

> [!NOTE]
> Where original project archives do not document an explicit historical meeting minute or author note, the rationale is documented as an **inferred engineering rationale** based on sound Operating Systems principles and the codebase implementation.

---

## Decision Matrix Summary

| # | Design Decision | Chosen Approach | Key Alternative | Category |
|---|---|---|---|---|
| 1 | **Language Standard** | C++17 | C99 / Rust | Language & Tooling |
| 2 | **Execution Domain** | Userspace Application | Linux Kernel Module (VFS) | System Architecture |
| 3 | **Storage Backing** | 64 MiB Regular File | Raw Block Partition (`/dev/loop`) | Storage Abstraction |
| 4 | **Block Size** | 4096 Bytes (4 KiB) | 512 Bytes / 1024 Bytes / 8 KiB | On-Disk Geometry |
| 5 | **Inode Size** | 256 Bytes | 128 Bytes / 512 Bytes | Inode Structure |
| 6 | **Inode Capacity** | Fixed 256 Inodes | Dynamic B-tree / Extents | Inode Table Design |
| 7 | **Allocation Tracking** | Bitmap (1 bit / item) | Free List / Extent Trees | Resource Allocation |
| 8 | **Root Inode ID** | Inode 1 | Inode 0 / Inode 2 | Namespace Design |
| 9 | **Reserved Inode 0** | Permanently Reserved | Usable Inode | Sentinel Protection |
| 10| **Addressing Model** | 16 Direct + 1 Single + 1 Double | Extents / Triple Indirect | Inode Addressing |
| 11| **Serialization** | Explicit Little-Endian | Struct Type-Casting / Big-Endian | Cross-Platform Portability |
| 12| **Superblock Magic** | `0x46494C4C` ("FILL") | No Magic / Checksum only | Volume Validation |
| 13| **Superblock Version** | Explicit `version = 1` | Unversioned | Format Evolution |
| 14| **Disk I/O Paradigm** | Positional I/O (`BlockDevice`) | Raw Global `lseek()` / `read()` | Storage Layering |
| 15| **Component Separation**| Dedicated Managers | Monolithic Filesystem Class | Software Modularity |
| 16| **User Interface** | Custom Interactive REPL Shell | CLI Args (`./filler mkdir`) | User Interaction |
| 17| **Metadata Durability**| Synchronous Write-Through | Write-Back Cache / Journaling | Reliability & Crash Safety|
| 18| **Crash Consistency** | Ordered Unlink Rollback | Write-Ahead Journaling (WAL) | Fault Tolerance |
| 19| **Kernel Integration** | Standalone Shell (No FUSE) | FUSE / Kernel VFS Mount | System Integration |

---

## 1. Programming Language: C++17

- **Decision**: Implement Filler in standard ISO C++17 using `-std=c++17` with strict compiler flags (`-Wall -Wextra -Wpedantic -Werror -O2 -g`).
- **Rationale**: Filesystem development requires low-level binary manipulation (bit shifting, byte offsets, buffer alignment) while simultaneously benefiting from modern RAII (Resource Acquisition Is Initialization), strong type safety (`enum class`), structured exceptions (`std::runtime_error`), and standard containers (`std::vector`, `std::array`).
- **Advantages**:
  - Deterministic destruction: `BlockDevice` closes file descriptors automatically when destructed.
  - Zero-cost abstractions: `std::array<uint64_t, 16>` has identical memory layout to raw C arrays without runtime penalty.
  - Structured error propagation: Throwing descriptive exceptions (`BlockBitmapError`, `NoFreeBlocksError`, `EntryNotFoundError`) is far cleaner than propagating error integers or checking `errno` across dozens of functions.
- **Alternatives Considered**:
  - *C99*: The traditional Unix filesystem language. *Rejected*: Lacks RAII, necessitating manual error unwinding (`goto error;`) which is notoriously error-prone during multi-stage rollbacks.
  - *Rust*: Exceptional memory and thread safety. *Rejected*: Higher barrier to entry for third-year CS curricula where C/C++ is the standard instructional language for Operating Systems.
- **Classification**: Deliberate engineering choice.

---

## 2. Userspace Implementation vs. Kernel Module

- **Decision**: Implement Filler as a standalone userspace process rather than an in-kernel VFS filesystem driver (`struct file_system_type`).
- **Rationale**: Developing kernel modules introduces massive friction: a single null pointer or out-of-bounds array access causes a kernel panic (BSOD/kernel oops), requiring a machine reboot. Debugging with `gdb`, running Valgrind, and automating unit test suites is vastly simpler in userspace.
- **Advantages**:
  - Full memory protection: Segmentation faults crash the process, not the host computer.
  - Complete test automation: All 113 unit and integration tests run in seconds without virtualization or root privileges.
  - Portability: Runs seamlessly on both Linux and macOS.
- **Trade-offs**: Does not allow native mounting via `mount -t filler filler.img /mnt`. Files can only be manipulated through Filler's CLI shell or internal C++ API.
- **Classification**: Deliberate architectural choice suited for educational and research systems.

---

## 3. Backing Storage: 64 MiB Flat Regular File

- **Decision**: Use a 64 MiB (67,108,864 bytes) sparse or pre-allocated regular file (`filler.img`) as the virtual disk image.
- **Rationale**:
  - 64 MiB is large enough to demonstrate realistic multi-block files, single-indirect addressing (> 64 KiB), double-indirect addressing (> 2 MiB), nested directory hierarchies, and bitmap exhaustion tests.
  - It is small enough to fit comfortably in RAM, commit cleanly to repositories, clone instantly in unit test temporary directories, and zero out rapidly during formatting.
- **Alternatives Considered**:
  - *Raw disk partition (`/dev/sda3`)*: Requires root privileges and risks accidental host data corruption.
  - *In-memory array (`std::vector<uint8_t>`)*: Fails to test true persistence across process exit and relaunch.
- **Classification**: Deliberate engineering choice.

---

## 4. Block Size: 4096 Bytes (4 KiB)

- **Decision**: Standardize on a fixed block size of 4096 bytes (`BlockDevice::BLOCK_SIZE = 4096`).
- **Rationale**:
  - 4096 bytes is the native virtual memory page size on x86-64 and modern ARM64 (Linux default). Aligning filesystem blocks with page frames minimizes page fault overhead and matches host OS block device I/O buffers.
  - At 4096 bytes, an indirect pointer block holds exactly \(4096 / 8 = 512\) 64-bit block pointers, producing clean powers-of-two addressing arithmetic.
- **Trade-offs**: Small files (e.g. 10-byte text files) waste \(4096 - 10 = 4086\) bytes due to **internal fragmentation**.
- **Alternatives Considered**:
  - *512 bytes*: Minimizes internal fragmentation for small files, but quadruples metadata overhead (larger bitmaps and indirect tables) and increases system call frequency by \(8\times\).
  - *8192 bytes (8 KiB)*: Increases internal fragmentation without tangible benefits for a 64 MiB volume.
- **Classification**: Standard industry practice (matches ext4, XFS, NTFS).

---

## 5. Inode Size: 256 Bytes

- **Decision**: Fixed 256-byte on-disk inode structure.
- **Rationale**:
  - 256 is an exact power of two (\(2^8\)).
  - A 4096-byte block holds exactly \(4096 / 256 = 16\) inodes without any wasted trailing bytes.
  - 256 bytes provides generous room for 16 direct block pointers (128 bytes), single and double indirect pointers (16 bytes), all standard metadata fields (size, type, permissions, uid, gid, atime, mtime, ctime = 64 bytes), and 48 reserved bytes for future enhancements (e.g. triple indirect pointers, checksums, ACLs).
- **Alternatives Considered**:
  - *128 bytes (classic ext2)*: Can only hold 12 direct pointers and lacks room for 64-bit nanosecond timestamps or future attributes.
  - *512 bytes*: Wastes 50% of the inode table space given Filler's feature set.
- **Classification**: Deliberate engineering choice.

---

## 6. Fixed Inode Capacity: 256 Inodes

- **Decision**: Pre-allocate an inode table with exactly 256 inodes (16 blocks = 64 KiB).
- **Rationale**:
  - For a 64 MiB filesystem, 256 inodes represents a balanced inode-to-data ratio. If each file averages \(\approx 250\) KiB, 256 files will completely utilize the 64 MiB volume.
  - Storing 256 inodes in 16 blocks keeps the inode table compact and ensures the entire inode bitmap fits in a single 4096-byte block (using only 32 bytes).
- **Trade-offs**: Once 254 user files and directories are created, the filesystem reports `NoFreeInodesError` even if tens of megabytes of data blocks remain free.
- **Alternatives Considered**:
  - *Dynamic Inode Allocation (like XFS or F2FS)*: Inodes allocated as normal data blocks on demand. *Rejected*: Dramatically increases complexity; requires B-trees or extents to locate inodes on disk.
- **Classification**: Deliberate simplification for educational clarity.

---

## 7. Allocation Mechanism: Bitmaps vs. Free Lists

- **Decision**: Use a 1-block Block Bitmap (Block 1) and a 1-block Inode Bitmap (Block 2).
- **Rationale**:
  - Bitmaps represent allocation state with the highest possible storage density: 1 bit per resource.
  - All 16,384 blocks of Filler are tracked in just 2048 bytes (half of Block 1).
  - Bitmaps allow immediate \(O(1)\) checks for allocation state (`is_allocated(id)`), fast sequential first-fit allocation, and trivial double-free detection.
- **Alternatives Considered**:
  - *Linked Free List*: Blocks form a linked list. *Rejected*: Free block allocation requires modifying pointers across scattered blocks, destroying spatial locality and complicating crash recovery.
  - *Extent Trees (ext4 / Btrfs)*: Track contiguous block ranges. *Rejected*: High algorithmic complexity unsuitable for a mini filesystem.
- **Classification**: Standard Unix filesystem design (ext2/ext3).

---

## 8. Root Inode ID = 1 and Reserved Inode ID = 0

- **Decision**: Assign Root Directory `/` to Inode 1; permanently reserve Inode 0.
- **Rationale**:
  - **Why reserve Inode 0?**: In C/C++, `0` is the canonical null/sentinel value. When an uninitialized or sparse data structure contains `0`, treating Inode 0 as invalid prevents uninitialized pointers from accidentally referencing a real file.
  - **Why Root = Inode 1?**: With Inode 0 reserved as a sentinel, Inode 1 is the first valid, assignable inode on the volume. Formatter assigns Inode 1 to the root directory `/`.
- **Note on Linux Ext2/3/4 Comparison**: Ext2/3/4 reserves Inodes 1 to 10 for system features (bad blocks, journal inode, etc.) and assigns Root to Inode 2. Filler does not have bad-block tracking or internal journaling, so starting the root directory at Inode 1 is a clean, minimal design.
- **Classification**: Deliberate design decision.

---

## 9. Inode Addressing Scheme: 16 Direct + 1 Single-Indirect + 1 Double-Indirect

- **Decision**: Equip each inode with 16 direct block pointers, 1 single-indirect pointer, and 1 double-indirect pointer. Omit triple-indirect pointers.
- **Rationale**:
  - 16 direct blocks cover files up to 64 KiB with zero indirection overhead.
  - The single-indirect pointer covers files up to \(\approx 2.06\) MiB with a single extra block read.
  - The double-indirect pointer provides theoretical capacity for \(512 \times 512 = 262,144\) blocks (1 GiB). Since Filler's entire volume is only 64 MiB (16,384 blocks), a double-indirect pointer alone is capable of addressing the entire physical disk image multiple times over.
  - A triple-indirect pointer would be completely redundant on a 64 MiB disk image.
- **Classification**: Optimal engineering fit for volume size.

---

## 10. Serialization Strategy: Explicit Little-Endian Helpers

- **Decision**: Prohibit raw memory type-casting (`reinterpret_cast<Superblock*>(buf)`); serialize and deserialize every integer explicitly using bitwise shifts in little-endian format.
- **Rationale**:
  - C++ compilers add padding bytes to structs according to host ABI alignment rules (`#pragma pack` is non-standard and causes unaligned memory traps on some architectures like ARM).
  - Explicit shift helpers (`val |= (src[i] << (i * 8))`) guarantee that an image created on an Intel x86-64 machine (little-endian) opens with 100% binary identical behavior on an Apple Silicon M-series ARM64 machine or a big-endian mainframe.
- **Classification**: Industry best practice for binary portability.

---

## 11. Superblock Magic Number (`0x46494C4C`) and Versioning

- **Decision**: Place the 4-byte magic number `0x46494C4C` at byte 0 and format version `1` at byte 4 of Block 0.
- **Rationale**:
  - Prevents mounting arbitrary binary files (ISO images, text documents, zip archives) as filesystems.
  - `0x46494C4C` translates directly to the ASCII string `"FILL"`. In little-endian memory, the bytes are stored as `0x4C, 0x4C, 0x49, 0x46` (`'L', 'L', 'I', 'F'`).
  - Version field allows future format revisions to be cleanly detected and rejected with an explicit `"unsupported version"` diagnostic instead of misinterpreting changed offsets as corruption.
- **Classification**: Standard filesystem specification requirement.

---

## 12. Positional I/O via `BlockDevice`

- **Decision**: Isolate all disk access into a dedicated `BlockDevice` class implementing strictly bounded 4096-byte positional reads and writes.
- **Rationale**:
  - Standard Unix `read()` and `write()` alter the implicit file offset pointer of the file descriptor. If multiple threads or nested calls share a descriptor, seek offsets can be invalidated.
  - `BlockDevice` ensures that every read and write specifies the target `block_id` explicitly, calculating the exact byte offset (`block_id * 4096`) and verifying that the buffer is at least 4096 bytes.
- **Classification**: Robust architectural separation of concerns.

---

## 13. Subsystem Modularity: Specialized Manager Classes

- **Decision**: Decompose the filesystem into distinct manager classes: `Superblock`, `BlockBitmap`, `InodeManager`, `DirectoryManager`, and `FileManager`.
- **Rationale**:
  - Conforms to the **Single Responsibility Principle** (SRP).
  - Enables isolated unit testing: `BlockBitmap` can be tested independently of `DirectoryManager`; `Superblock` can be tested without allocating files.
  - Eliminates circular dependencies: `FileManager` depends on `DirectoryManager`, which depends on `InodeManager`, which depends on `BlockDevice`.
- **Classification**: Modular software engineering architecture.

---

## 14. Custom Interactive Shell vs. CLI Subcommands

- **Decision**: Provide a persistent interactive REPL shell (`filler:/dir> `) with an integrated command interpreter.
- **Rationale**:
  - Running commands like `./filler cd folder && ./filler ls` cannot work across separate process invocations because each process exits, losing the current working directory (`cwd`) context.
  - An interactive shell allows maintaining in-memory mount state, cached metadata, and current directory tracking across consecutive user commands.
- **Classification**: Usability and architectural necessity.

---

## 15. Synchronous Write-Through vs. Write-Back Buffer Cache

- **Decision**: Filler implements a **synchronous write-through** policy for all metadata operations: modifications to bitmaps, inodes, and superblocks are immediately flushed to the `BlockDevice`.
- **Rationale**:
  - Write-back caching requires complex dirty-buffer management, background flusher threads (`flusher` / `jbd2`), and cache-eviction algorithms (LRU).
  - In an educational filesystem running on top of host OS files, the host OS kernel already caches disk blocks in its own page cache. Implementing a second cache layer inside userspace adds massive complexity with negligible performance gains.
- **Trade-offs**: More frequent POSIX `write()` system calls.
- **Classification**: Deliberate simplification for safety and simplicity.

---

## 16. Crash Consistency & Atomicity: Rollback Context vs. Journaling

- **Decision**: Filler does **not** implement a Write-Ahead Journal (WAL). To protect consistency during multi-block operations, Filler uses:
  1. **Truncation Rollback Context**: In-memory tracking of all newly allocated blocks and backups of modified indirect tables, rolling them back if allocation fails midway.
  2. **Ordered Directory Deletion**: Removing the directory entry *before* deallocating inode and data blocks.
- **Trade-offs**:
  - If a hard power loss occurs midway between directory unlinking and block freeing, the unlinked file's blocks and inode remain allocated in the bitmaps (an "orphan" resource).
  - *Why this is acceptable*: An orphan resource is a harmless temporary space leak recoverable by an offline `fsck`. The reverse order (freeing blocks first) would risk critical data corruption where an active directory entry points to blocks reallocated to another file.
- **Why Journaling was Omitted**: Journaling doubles write amplification and requires transaction IDs, log descriptors, circular log replay, and checkpointing engines.
- **Classification**: Honest technical limitation and deliberate educational scope boundary.

---

## 17. No FUSE (Filesystem in Userspace) Integration

- **Decision**: Implement Filler as a standalone application rather than a FUSE driver (`libfuse`).
- **Rationale**:
  - FUSE requires installing kernel extensions (`macFUSE` on macOS, `fuse3` on Linux), elevated root permissions, and platform-specific compilation flags.
  - FUSE hides the command dispatching, path resolution, and shell mechanics that are vital topics in Operating Systems education.
- **Classification**: Scope boundary.

