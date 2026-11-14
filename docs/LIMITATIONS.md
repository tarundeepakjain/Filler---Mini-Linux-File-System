# Filler – Honest Technical Limitations & Future Roadmap

This document provides a candid, technically rigorous assessment of the architectural and operational limitations of **Filler**. In accordance with professional engineering standards, we clearly differentiate implemented capabilities from future enhancements, detailing the structural cause of each limitation and the engineering roadmap required to resolve it.

---

## 1. System Integration & Execution Limitations

### 1.1 No Kernel VFS Integration
- **The Limitation**: Filler cannot be mounted natively using the Linux or macOS `mount` command (e.g. `mount -t filler filler.img /mnt`). It cannot be accessed by standard OS tools like `cat`, `grep`, `nano`, `vim`, `ls`, or `cp`.
- **Why It Exists**: Filler is implemented purely in userspace as a standalone C++17 binary. It does not implement a Linux kernel module (`struct file_system_type`, `struct super_operations`, `struct inode_operations`).
- **Roadmap to Address**: Write a Linux Loadable Kernel Module (LKM) in C registering with the Linux VFS layer, or develop an eBPF-based userspace file system bridge.

### 1.2 No FUSE (Filesystem in Userspace) Driver
- **The Limitation**: The filesystem cannot be mounted in userspace via `/dev/fuse`.
- **Why It Exists**: Filler prioritizes standalone educational clarity, eliminating external build dependencies (`libfuse`, `macFUSE`) and avoiding the complex multi-threaded asynchronous callbacks of the FUSE event loop.
- **Roadmap to Address**: Implement a `FuseOperations` wrapper class mapping `fuse_lowlevel_ops` or `fuse_operations` (`open`, `read`, `write`, `readdir`, `mkdir`, `unlink`) to `FileManager` and `DirectoryManager` methods.

---

## 2. Fault Tolerance & Consistency Limitations

### 2.1 No Journaling or Write-Ahead Logging (WAL)
- **The Limitation**: Multi-block operations (such as creating a file, writing multiple blocks, or deleting an entry) are not transactional. If the host machine experiences a sudden power loss or process kill (`kill -9`) midway through an operation, the filesystem can be left in an inconsistent state.
- **Why It Exists**: Implementing a journal (like ext4's JBD2) requires a dedicated circular log area on disk, transaction descriptor blocks, commit records, log checkpointing, and a replay engine on startup.
- **Current Safeguards**: Filler mitigates inconsistency through:
  - **In-memory truncate rollback**: Partial allocations during file growth are caught and cleanly rolled back before metadata is persisted.
  - **Ordered unlinking**: Directory entries are unlinked *before* releasing data blocks and inodes.
- **Roadmap to Address**: Reserve Blocks 19–34 as a dedicated circular transaction log. Implement two-phase commit: write modified metadata blocks to the log first, append a commit record, then flush to the in-place filesystem blocks.

### 2.2 Lack of an Automated `fsck` Recovery Tool
- **The Limitation**: If a crash leaves "orphaned" blocks or inodes (resources marked allocated in bitmaps but unreferenced by any directory), Filler provides no automated tool to scan, identify, and recover them into a `lost+found` directory.
- **Roadmap to Address**: Develop a standalone `filler-fsck` utility that reads the superblock, verifies bitmap bit counts against a complete traversal of the directory tree, fixes discrepancies, and clears unreferenced inodes.

---

## 3. Capacity & Layout Details

### 3.1 Dynamic Capacity & Inode Scaling (Implemented)
- **Status: Implemented**: Filler now dynamically calculates volume geometry, bitmap block counts, and scalable Inode tables for arbitrary device capacities—from 64 MiB up to multi-gigabyte disks and physical USB flash drives (such as an HP 16 GB USB drive).
- **Default Scaling**: Allocates 1 inode per 256 KiB of storage capacity (\(I = \max(256, T / 64)\) rounded to 16 inodes). For a 16 GB flash drive, this provisions ~61,040 inodes while keeping metadata overhead below 0.1%.
- **Future Enhancement**: Allow user-specified inode densities via CLI arguments (e.g. `--inodes <count>` or `-i <bytes-per-inode>`).

### 3.2 Physical Storage & Flash Memory Considerations
- **Flash Translation Layer (FTL)**: Physical USB drives use NAND flash memory. While wear leveling and bad-block remapping are handled internally by the USB drive's hardware microcontroller (FTL), Filler performs direct 4096-byte block I/O without host write caching.
- **No TRIM / Discard Support**: When files or blocks are freed in Filler, the bitmap bits are cleared, but no `ioctl(BLKDISCARD)` command is sent to the physical USB flash drive to notify the FTL that blocks are no longer in use.
- **Roadmap to Address**: Issue `BLKDISCARD` ioctls when freeing large contiguous extents on SSDs or flash drives supporting discard.

### 3.3 Directory Capacity Limit (64 KiB / 16 Direct Blocks)
- **The Limitation**: A directory in Filler can only allocate up to 16 direct blocks (`65,536` bytes). It does not use indirect pointers.
- **Why It Exists**: Directories rarely contain thousands of entries in a mini filesystem, and direct block traversal simplifies `DirectoryManager::read_entries()`.
- **Practical Impact**: Assuming an average entry length of 24 bytes, a single directory can hold at most \(\approx 2,700\) files.
- **Roadmap to Address**: Extend `DirectoryManager` to use `FileManager`'s indirect block addressing routines.

---

## 4. Concurrency & Security Limitations

### 4.1 Lack of Thread-Safety & Multi-Process Concurrency
- **The Limitation**: The C++ classes (`BlockDevice`, `BlockBitmap`, `FileManager`) contain no mutexes, read-write locks (`std::shared_mutex`), or POSIX file locks (`fcntl(F_SETLK)`). Accessing the same disk image concurrently from multiple threads or separate terminal processes causes race conditions and bitmap corruption.
- **Roadmap to Address**: Add an `std::mutex` inside `BlockDevice` for thread safety, and acquire an advisory POSIX file lock (`flock()` or `fcntl`) upon opening `filler.img` to prevent concurrent process launches.

### 4.2 Permissions and Access Control Are Not Enforced
- **The Limitation**: Although `Inode` contains fields for `permissions` (`0644`, `0755`), `uid`, and `gid`, the `Shell` and `FileManager` do not check user permissions. Any command executed in the shell has unrestricted read, write, and delete access to all files.
- **Roadmap to Address**: Add user authentication (`login`, `su`), track current user/group ID in `Shell`, and verify read/write/execute bitmasks before executing file operations.

### 4.3 No Support for Hard Links or Symbolic Links
- **The Limitation**:
  - **No Hard Links**: Inodes do not track a reference count (`nlink`). Creating multiple directory entries pointing to the same inode is unsupported.
  - **No Symbolic Links**: There is no `FileType::SYMLINK` or path redirection mechanism.
- **Roadmap to Address**: Add `uint32_t nlink` to `Inode` (utilizing 4 bytes of `reserved`), increment `nlink` on link creation, and only release inode/blocks when `nlink == 0`. Add `FileType::SYMLINK` and store target path strings in data blocks.

