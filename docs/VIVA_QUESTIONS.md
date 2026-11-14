# Filler – Comprehensive Viva & Interview Preparation Guide

This document contains **55 deeply technical questions and rigorous answers** designed to prepare students and engineers for an Operating Systems viva, technical defense, or systems engineering interview based on **Filler – A Custom Userspace Mini File System**.

---

## Category 1: Filesystem Fundamentals

### Q1: What is a filesystem, and why is it necessary in an operating system?
**Answer**: A filesystem is an operating system abstraction that manages how data is stored, organized, named, and retrieved on non-volatile secondary storage. Without a filesystem, storage is merely a continuous array of raw sectors. Applications would need to track physical disk offsets manually, risking overwrite conflicts, lacking hierarchical organization, and having no mechanism to manage file boundaries, metadata, or free space.

### Q2: What does "userspace filesystem" mean, and how does Filler differ from an in-kernel filesystem like ext4?
**Answer**: An in-kernel filesystem (like ext4 or XFS) executes in Ring 0 (kernel space) as a VFS module. It hooks into hardware interrupts, registers with the Virtual Filesystem Switch, and can be mounted natively to the OS directory tree (`/mnt`). 

A userspace filesystem executes in Ring 3 (user space) as a standard process. Filler does not hook into the kernel VFS; instead, it provides its own shell interface and manipulates a virtual disk image (`filler.img`) using standard POSIX system calls (`open`, `read`, `write`, `lseek`). It demonstrates all the core algorithmic principles of a filesystem (bitmaps, superblocks, inodes, indirect addressing) without the danger of crashing the host kernel.

### Q3: What is internal fragmentation, and how does it manifest in Filler?
**Answer**: Internal fragmentation occurs when allocated storage units are larger than the payload data stored within them, leaving unused "slack" space inside the allocated block. In Filler, the block size is 4096 bytes. If a user creates a file containing 10 bytes ("Hello!"), Filler must allocate an entire 4096-byte block to hold it. The remaining \(4096 - 10 = 4086\) bytes are unused, representing internal fragmentation.

### Q4: What is external fragmentation, and why does Filler not suffer from it?
**Answer**: External fragmentation occurs when total free space is sufficient to satisfy an allocation request, but the space is divided into small, non-contiguous fragments so that a contiguous allocation cannot be fulfilled. Because Filler uses an indexed block allocation scheme (inodes with direct and indirect pointers), files **do not need to occupy contiguous physical blocks**. Any arbitrary free block on the disk can be assigned to any logical position in a file. Therefore, Filler suffers zero external fragmentation.

---

## Category 2: Block Devices & Storage Abstraction

### Q5: What is a block device, and how does it differ from a character device?
**Answer**: A **block device** transfers data in fixed-size atomic chunks (blocks), supports random access, and allows arbitrary seeking to any block address (e.g. hard drives, SSDs, disk images). A **character device** transfers unbuffered streams of bytes sequentially without random seeking (e.g. keyboards, serial ports, mice).

### Q6: What is the difference between a physical sector and a filesystem block?
**Answer**: A **sector** is the smallest physical transfer unit defined by the underlying hardware controller (historically 512 bytes, now commonly 4096 bytes in Advanced Format drives). A **block** is a logical software allocation unit defined by the filesystem (e.g. 4096 bytes in Filler). A filesystem block is always an integer multiple of the hardware sector size.

### Q7: Why did Filler introduce a dedicated `BlockDevice` class rather than calling POSIX `read()` and `write()` everywhere?
**Answer**: 
1. **Encapsulation & Safety**: It guarantees that every I/O operation is strictly validated for boundary correctness (`block_id < 16384`), buffer non-nullness, and exact 4096-byte transfer sizes.
2. **Positional Independence**: Standard `read()` and `write()` modify the file descriptor's seek offset. `BlockDevice` computes exact byte offsets (`block_id * 4096`), ensuring race-free, deterministic addressing.
3. **Hardware Isolation**: Higher-level managers (`InodeManager`, `DirectoryManager`, `FileManager`) depend on a clean, generic block interface rather than low-level POSIX primitives or `errno`.

### Q8: How does Filler map a `block_id` to a byte offset in the backing file?
**Answer**: 
\[
\text{ByteOffset} = \text{block\_id} \times \text{BLOCK\_SIZE} = \text{block\_id} \times 4096
\]
For example, Block 19 starts at \(19 \times 4096 = 77,824\) bytes (`0x13000`).

---

## Category 3: Superblock & Formatting

### Q9: What is a Superblock, and why is it located at Block 0?
**Answer**: The Superblock is the master descriptor of the filesystem. It records the geometry, magic number, version, block size, inode count, dynamic free resource counts, and regional block boundaries. It is located at Block 0 because a mounting system must know where the metadata is located before it can interpret any other block on the volume. Block 0 is the universal fixed entry point.

### Q10: What is the significance of the magic number `0x46494C4C`?
**Answer**: The magic number uniquely identifies the disk image as a Filler filesystem volume. In ASCII hex, `0x46` = `'F'`, `0x49` = `'I'`, `0x4C` = `'L'`, `0x4C` = `'L'`, spelling the word `"FILL"`. In little-endian storage on disk, the bytes appear in reverse: `0x4C, 0x4C, 0x49, 0x46` (`'L', 'L', 'I', 'F'`).

### Q11: What happens if the Superblock magic number is corrupted or set to zero?
**Answer**: During startup, `Shell::inspect_filesystem()` checks the magic number:
- If magic is `0` and the rest of Block 0/bitmap is zero: Classified as `UNFORMATTED`.
- If magic is `0` but existing metadata is present: Classified as `CORRUPTED`.
- If magic is nonzero but not `0x46494C4C`: Classified as `INVALID_IMAGE`.
Filler refuses to mount the filesystem and outputs a descriptive error. It **never** silently formats or overwrites a corrupted image.

### Q12: Exactly what steps are performed when `format` is executed?
**Answer**:
1. Verifies/resizes image to 64 MiB (16,384 blocks).
2. Writes default Superblock to Block 0.
3. Initializes Block Bitmap (Block 1), marking Blocks 0..18 as allocated.
4. Initializes Inode Bitmap (Block 2), marking Inodes 0 and 1 as allocated.
5. Zeroes out all 16 blocks of the Inode Table (Blocks 3..18).
6. Allocates Block 19 to Root Inode 1 and populates it with directory entries `.` and `..`.

---

## Category 4: Bitmaps & Space Allocation

### Q13: What is a bitmap, and why is it preferred over a linked list for block allocation?
**Answer**: A bitmap is an array of bits where each bit represents the binary allocation state of a resource (`0` = free, `1` = allocated). Bitmaps provide \(O(1)\) allocation state checks, high spatial locality, compact storage (1 bit per block), and trivial double-free detection. Linked free lists scatter pointers across data blocks, making sequential block allocation slow and crash recovery difficult.

### Q14: How many blocks does Filler's Block Bitmap require, and how much space is unused?
**Answer**: Total blocks = 16,384. At 1 bit per block, the bitmap requires \(16,384 \text{ bits} = 2048 \text{ bytes}\). Since a disk block is 4096 bytes, the block bitmap occupies exactly **1 block** (Block 1). The first 2048 bytes track all 16,384 blocks; the remaining 2048 bytes are unused and zeroed.

### Q15: How does Filler calculate which byte and bit correspond to Block ID 150?
**Answer**:
- Byte Index: \(\lfloor 150 / 8 \rfloor = 18\)
- Bit Offset: \(150 \pmod{8} = 6\)
- Bit Mask: \(1 \ll 6 = 0x40\) (`0b01000000`)
Block 150 is tracked by Bit 6 of Byte 18 in Block 1.

### Q16: What blocks are permanently marked as allocated during formatting?
**Answer**: Blocks 0 through 18 (19 blocks total), covering:
- Block 0: Superblock
- Block 1: Block Bitmap
- Block 2: Inode Bitmap
- Blocks 3–18: Inode Table (16 blocks)
These metadata blocks are reserved to prevent user files from overwriting core filesystem structures.

### Q17: What error is raised when the Block Bitmap runs out of free blocks?
**Answer**: `filler::fs::NoFreeBlocksError`, which inherits from `BlockBitmapError` and `std::runtime_error`.

---

## Category 5: Inodes & Inode Table

### Q18: What is an Inode?
**Answer**: An Inode (index node) is a fixed-size on-disk control block that stores all metadata about a filesystem object (file or directory) except its name. It contains file size, permissions, user/group IDs, timestamps, link counts, and physical block pointers.

### Q19: Why are filenames stored in directory entries rather than inside the inode?
**Answer**: 
1. **Fixed Inode Size**: Storing variable-length filenames in inodes would make inode structures variable in size, preventing fast direct arithmetic indexing (`offset = inode_id * 256`).
2. **Hard Links**: Decoupling names from inodes allows multiple directory entries across different directories to reference the exact same inode.

### Q20: What are the exact dimensions of the Inode Table in Filler?
**Answer**:
- Inode Size: 256 bytes
- Total Inodes: 256
- Total Table Size: \(256 \times 256 = 65,536\) bytes (64 KiB)
- Inodes per Block: \(4096 / 256 = 16\) inodes
- Total Blocks: \(65,536 / 4096 = 16\) blocks (Blocks 3 through 18).

### Q21: How do you calculate the exact physical disk location of Inode 35?
**Answer**:
1. \(\text{RelativeBlock} = \lfloor 35 / 16 \rfloor = 2\)
2. \(\text{PhysicalBlock} = \text{inode\_table\_start} + \text{RelativeBlock} = 3 + 2 = \mathbf{5}\)
3. \(\text{BlockOffset} = (35 \pmod{16}) \times 256 = 3 \times 256 = \mathbf{768}\) bytes.
4. \(\text{AbsoluteByteOffset} = (5 \times 4096) + 768 = 20,480 + 768 = \mathbf{21,248}\) (`0x5300`).

### Q22: Why is Inode 0 reserved, and what is Root Inode's ID?
**Answer**: Inode 0 is permanently reserved as a null/sentinel value to catch uninitialized pointers. Inode 1 is assigned to the root directory `/`.

---

## Category 6: Direct & Indirect Block Addressing

### Q23: Explain the difference between direct, single-indirect, and double-indirect block pointers.
**Answer**:
- **Direct Pointer**: Directly stores the physical block ID of a user data block.
- **Single-Indirect Pointer**: Stores the block ID of an *indirect pointer table* block, which contains an array of direct block pointers.
- **Double-Indirect Pointer**: Stores the block ID of a *Level-1 pointer table*, which points to multiple *Level-2 pointer tables*, which in turn point to data blocks.

### Q24: How many pointers fit in a single 4096-byte indirect block in Filler?
**Answer**: Each block pointer is a 64-bit integer (`uint64_t`, 8 bytes).
\[
\text{Pointers per Block} = \frac{4096 \text{ bytes}}{8 \text{ bytes}} = \mathbf{512} \text{ pointers}
\]

### Q25: What is the maximum file size supported by Filler's direct blocks alone?
**Answer**: Filler inodes have 16 direct block pointers.
\[
\text{Direct Capacity} = 16 \times 4096 \text{ bytes} = 65,536 \text{ bytes} = \mathbf{64} \text{ KiB}
\]

### Q26: What is the cumulative capacity of direct blocks plus the single-indirect block?
**Answer**:
- Direct blocks: 16 blocks = 64 KiB
- Single-indirect: 512 blocks = \(512 \times 4096 = 2,097,152\) bytes = 2 MiB
- Cumulative capacity: \(65,536 + 2,097,152 = \mathbf{2,162,688}\) bytes (\(\approx 2.06\) MiB).

### Q27: What is the theoretical maximum file size supported by Filler's inode addressing?
**Answer**:
- Direct: 16 blocks
- Single Indirect: 512 blocks
- Double Indirect: \(512 \times 512 = 262,144\) blocks
- Total blocks = \(16 + 512 + 262,144 = 262,672\) blocks
- Theoretical size: \(262,672 \times 4096 = \mathbf{1,075,904,512}\) bytes (\(\approx 1.002\) GiB).
*(Note: Practical file size is bounded by the 64 MiB disk image capacity).*

### Q28: How does Filler map logical byte offset 100,000 to a physical block?
**Answer**:
1. \(\text{LogicalBlock} = \lfloor 100,000 / 4096 \rfloor = 24\)
2. \(\text{OffsetInBlock} = 100,000 \pmod{4096} = 1696\)
3. Since \(16 \le 24 < 528\), it falls into the **single-indirect region**.
4. \(\text{IndexInTable} = 24 - 16 = \mathbf{8}\).
5. Read table at `inode.single_indirect`, extract pointer at index 8 (byte offset \(8 \times 8 = 64\)). That pointer is the target physical block.

---

## Category 7: Directories & Path Resolution

### Q29: How does Filler represent a directory entry on disk?
**Answer**: As a packed 4-byte aligned binary record:
- `inode_id`: 8 bytes (`uint64_t`)
- `type`: 4 bytes (`uint32_t`, `1` = regular, `2` = directory)
- `name_len`: 4 bytes (`uint32_t`)
- `name`: `name_len` bytes (raw ASCII)
- `padding`: 0 to 3 zero bytes to align record length to a 4-byte boundary.

### Q30: What are `.` and `..` directory entries, and why are they necessary?
**Answer**: `.` represents the current directory, and `..` represents the parent directory. They allow hierarchical relative navigation (`cd ..`, `cd ./docs`) and maintain tree traversals. In Root Inode 1, both `.` and `..` point to Inode 1.

### Q31: How does path resolution work for a relative path like `../docs/plan.txt`?
**Answer**:
1. Starts at `current_dir_inode_`.
2. Resolves `".."`, looking up the parent inode.
3. In the parent directory, looks up `"docs"` to find the sub-directory inode.
4. In `"docs"`, looks up `"plan.txt"` to find the file's target inode.
5. Canonicalizes the path string to update the shell prompt.

### Q32: What is the maximum size of a directory in Filler?
**Answer**: Directories in Filler use direct blocks only (`direct_blocks[0..15]`). Thus, the maximum directory size is \(16 \times 4096 = \mathbf{64}\) KiB.

---

## Category 8: File Operations & I/O

### Q33: What happens when a user reads past the end of a file (EOF)?
**Answer**: `FileManager::read()` returns `0`, matching standard POSIX `read()` EOF semantics.

### Q34: What is a sparse file, and how does Filler handle writes beyond the current EOF?
**Answer**: A sparse file contains unwritten gaps (holes). In Filler, if a file has size 0 and a user writes 10 bytes at offset 10,000:
- Blocks covering offsets 0..8191 are not allocated (their pointers remain `0`).
- The block covering offset 10,000 is allocated.
- When reading from offsets 0..8191, `FileManager::read()` detects physical block `0` and populates the user buffer with zeroes without reading from disk.

### Q35: What happens when a file is truncated to a smaller size?
**Answer**:
1. Trailing blocks beyond the new size are deallocated via `BlockBitmap::free()`.
2. If `new_size % 4096 != 0`, the unused trailing bytes of the last remaining block are zero-filled to prevent leaking old data.
3. Inode `size_bytes` is updated and persisted to disk.

---

## Category 9: Consistency, Fault Tolerance & Rollback

### Q36: What is the "allocation rollback" problem during file growth?
**Answer**: When a file grows, it may require multiple data blocks and indirect tables. If the disk fills up midway (e.g. after allocating 2 of 5 required blocks), leaving those 2 blocks allocated leaks space, and writing partial pointers corrupts the file.

### Q37: How does Filler solve the allocation rollback problem?
**Answer**: Through `TruncateRollbackContext`:
1. Modifications are staged on an in-memory copy of the inode.
2. Every newly allocated block is tracked in a vector.
3. Existing indirect pointer tables are backed up in memory before being modified.
4. If an exception occurs, all modified tables are restored from backup to disk, all newly allocated blocks are freed in the bitmap, and the inode on disk is never updated.

### Q38: In what order does Filler delete a file, and why?
**Answer**: 
1. **Unlink directory entry first**.
2. **Deallocate data and indirect blocks second**.
3. **Deallocate inode third**.
*Reason*: If deletion fails or power is lost after unlinking, an unreferenced "orphan" resource is created (a harmless space leak reclaimable by `fsck`). If blocks were freed first, an active directory entry would point to freed blocks, risking catastrophic data corruption if another file reallocates them.

### Q39: Does Filler have ACID transaction atomicity?
**Answer**: No. Filler does not have a write-ahead journal (WAL). While it guarantees in-memory rollback during application exceptions, an abrupt host OS crash or power cut midway through a multi-block write can leave orphan resources.

---

## Category 10: C++ & POSIX Implementation

### Q40: Why does Filler use RAII for `BlockDevice`?
**Answer**: Resource Acquisition Is Initialization (RAII) ensures that the POSIX file descriptor `fd_` is opened in the constructor and closed in the destructor. Copy constructors are deleted (`= delete`) to prevent multiple instances from closing the same file descriptor.

### Q41: Why does Filler avoid `reinterpret_cast` of memory structures to disk buffers?
**Answer**: Direct casting of structs (`struct Superblock*`) is vulnerable to:
1. Compiler structure padding differences.
2. CPU endianness mismatches (x86 little-endian vs big-endian).
Explicit bit-shift serialization guarantees identical on-disk layout across all compilers and architectures.

### Q42: What POSIX system calls are used by Filler?
**Answer**:
- `open()`: Opens or creates the disk image (`O_RDWR`).
- `close()`: Closes the file descriptor.
- `lseek()`: Sets exact byte offsets for positional block I/O.
- `read()` & `write()`: Transfers block data.
- `fstat()`: Validates image size and ensures it is a regular file.
- `ftruncate()`: Pre-allocates or resizes the disk image file.

---

## Category 11: Architecture & Design Decisions

### Q43: Why was 64 MiB chosen for the disk image size?
**Answer**: It is large enough to demonstrate multi-level indirect addressing and realistic workloads, yet small enough to execute all 113 unit tests in under 2 seconds without consuming host disk space.

### Q44: Why does Filler implement a custom shell instead of single-shot CLI commands?
**Answer**: A shell maintains persistent in-memory session state (such as the current working directory `current_path_` and cached volume structures). Running separate CLI commands (`filler cd docs`) would lose context between process invocations.

### Q45: How does Filler ensure neighboring inodes in the same block are not corrupted?
**Answer**: Through a **Read-Modify-Write** cycle: `InodeManager` reads the full 4096-byte block holding the 16 inodes, modifies only the 256-byte slice corresponding to the target inode, and writes the entire 4096-byte block back to disk.

---

## Category 12: Limitations & Future Enhancements

### Q46: What are the three most significant limitations of Filler?
**Answer**:
1. No native kernel mount or FUSE driver (accessible only via Filler shell).
2. No write-ahead journaling (vulnerable to crashes midway through multi-block I/O).
3. Fixed inode capacity of 256 inodes (cannot exceed 254 user files).

### Q47: How could FUSE support be added to Filler?
**Answer**: By creating a wrapper that implements `fuse_operations` callbacks (`.getattr`, `.readdir`, `.open`, `.read`, `.write`, `.mkdir`, `.unlink`) that delegate directly to `FileManager` and `DirectoryManager`.

### Q48: How could journaling be added to Filler?
**Answer**: By reserving a contiguous set of blocks (e.g. Blocks 19–34) as a circular transaction log. Metadata writes would first be appended to the log with a commit marker before being written to their in-place filesystem blocks.

### Q49: Why does Filler lack file permissions enforcement even though the inode stores permissions?
**Answer**: The fields exist to demonstrate standard Unix inode structures, but access control enforcement requires multi-user authentication (`login`, `su`, `/etc/passwd`), which was out of scope for a single-user mini filesystem shell.

### Q50: How would you implement hard links in Filler?
**Answer**:
1. Add an `nlink` reference count field to `Inode`.
2. When creating a link, increment `inode.nlink` and add the directory entry.
3. In `FileManager::remove()`, decrement `inode.nlink`. Only free blocks and the inode when `nlink == 0`.

---

## Category 13: Numerical & Geometry Calculations

### Q51: How many total blocks exist on a 64 MiB image with 4096-byte blocks?
**Answer**:
\[
\text{Total Blocks} = \frac{64 \times 1024 \times 1024 \text{ bytes}}{4096 \text{ bytes}} = \frac{67,108,864}{4096} = \mathbf{16,384} \text{ blocks}
\]

### Q52: How many user data blocks are available on a freshly formatted Filler image?
**Answer**:
- Total Blocks = 16,384
- Metadata Blocks = 19 (Blocks 0..18)
- Root Directory Block = 1 (Block 19)
- Free Data Blocks = \(16,384 - 19 - 1 = \mathbf{16,364}\) blocks (\(\approx 67,002,368\) bytes \(\approx 63.89\) MiB).

### Q53: If a file has size 12,288 bytes, how many blocks does it occupy?
**Answer**:
\[
\text{Blocks} = \frac{12,288 \text{ bytes}}{4096 \text{ bytes/block}} = \mathbf{3} \text{ blocks (Blocks 0, 1, 2 in direct\_blocks)}
\]

### Q54: If a file has size 70,000 bytes, how many data blocks and metadata blocks does it use?
**Answer**:
- Data blocks: \(\lceil 70,000 / 4096 \rceil = \mathbf{18}\) data blocks.
- 16 data blocks fit in `direct_blocks[0..15]`.
- The remaining 2 data blocks require allocating 1 `single_indirect` pointer table block.
- Total blocks allocated = \(18 \text{ data blocks} + 1 \text{ table block} = \mathbf{19}\) blocks (`blocks_count = 19`).

### Q55: How many bits and bytes are needed to track 16,384 blocks in a bitmap?
**Answer**:
- Bits needed = 16,384 bits.
- Bytes needed = \(16,384 / 8 = \mathbf{2048}\) bytes.
- This occupies exactly half of one 4096-byte block.

