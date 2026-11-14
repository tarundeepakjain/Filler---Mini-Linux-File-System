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

## 3. Capacity & Layout Limitations

### 3.1 Fixed Inode Table Capacity (256 Inodes)
- **The Limitation**: The filesystem can store at most 254 user files and directories (Inodes 0 and 1 are reserved). Once 254 inodes are exhausted, creating new files fails with `NoFreeInodesError`, even if tens of megabytes of free space remain in the data region.
- **Why It Exists**: The Inode Table is statically sized at 16 blocks (65,536 bytes) during formatting to maintain a simple, predictable layout without dynamic B-trees.
- **Roadmap to Address**: Support variable formatting options (`filler -N 1024`) or implement dynamic inode allocation where inodes are allocated from the data region on demand.

### 3.2 Fixed Volume Size (64 MiB Geometry)
- **The Limitation**: Filler expects an exact 64 MiB (16,384 blocks) image layout. It cannot format or mount 1 GiB or 100 GiB disks.
- **Why It Exists**: Simplifies calculations, bounds checking, and ensures deterministic test behavior.
- **Roadmap to Address**: Generalize `Formatter::format()` and `Superblock::create_default()` to compute bitmap blocks and inode table blocks dynamically based on `device.size_bytes()`.

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

