# Filler – Exact Disk Layout & Storage Architecture

This document specifies the exact physical on-disk layout, memory structures, addressing calculations, and mapping formulas implemented by the **Filler** userspace file system. Every offset, size, and layout boundary documented here is verified directly against the C++17 source code (`include/fs/Superblock.hpp`, `include/fs/Inode.hpp`, `include/fs/Formatter.hpp`, and `include/storage/BlockDevice.hpp`).

---

## 1. Physical Disk Specifications

Filler operates on a 64 MiB flat binary disk image file (`filler.img`). The backing file is divided into uniform 4096-byte blocks.

| Parameter | Value | Value in Hex / Bytes | Source Constant |
|---|---|---|---|
| **Disk Image Size** | 64 MiB | 67,108,864 bytes (`0x04000000`) | `Formatter::DEFAULT_DISK_SIZE` |
| **Block Size** | 4096 bytes (4 KiB) | 4,096 bytes (`0x1000`) | `BlockDevice::BLOCK_SIZE` |
| **Total Blocks** | 16,384 blocks | `0x4000` | `Formatter::DEFAULT_TOTAL_BLOCKS` |
| **Inode Size** | 256 bytes | `0x0100` | `Inode::INODE_SIZE` |
| **Total Inodes** | 256 inodes | `0x0100` | `Formatter::DEFAULT_TOTAL_INODES` |
| **Inodes per Block** | 16 inodes | 4096 / 256 = 16 | `BlockDevice::BLOCK_SIZE / Inode::INODE_SIZE` |
| **Root Inode Number** | 1 | `0x00000001` | `Superblock::DEFAULT_ROOT_INODE` |
| **Reserved Inode Number** | 0 | `0x00000000` | `Superblock::RESERVED_INODE` |
| **Superblock Block ID** | 0 | `0x00000000` | `Superblock::SUPERBLOCK_BLOCK_ID` |
| **Endianness** | Little-endian | Least-significant byte first | Explicit serialization helpers |

---

## 2. Global Disk Region Map

The 16,384 blocks are divided into five contiguous functional regions without any padding or gaps:

```text
+--------------+---------------+---------------+-------------------+----------------------+
| Superblock   | Block Bitmap  | Inode Bitmap  | Inode Table       | Data Region          |
| Block 0      | Block 1       | Block 2       | Blocks 3–18       | Blocks 19–16,383     |
| (1 block)    | (1 block)     | (1 block)     | (16 blocks)       | (16,365 blocks)      |
+--------------+---------------+---------------+-------------------+----------------------+
0 KiB          4 KiB           8 KiB           12 KiB              76 KiB                 64 MiB
```

### Region Allocation Summary

| Region | Starting Block | Ending Block | Block Count | Byte Offset Range | Purpose |
|---|---:|---:|---:|---|---|
| **Superblock** | 0 | 0 | 1 | `0x00000000` – `0x00000FFF` | Filesystem configuration, magic number, layout pointers |
| **Block Bitmap** | 1 | 1 | 1 | `0x00001000` – `0x00001FFF` | Allocation status of all 16,384 blocks |
| **Inode Bitmap** | 2 | 2 | 1 | `0x00002000` – `0x00002FFF` | Allocation status of all 256 inodes |
| **Inode Table** | 3 | 18 | 16 | `0x00003000` – `0x00012FFF` | Array of 256 serialized `Inode` structures (256 B each) |
| **Data Region** | 19 | 16,383 | 16,365 | `0x00013000` – `0x03FFFFFF` | Storage for regular file content and directory entries |

---

## 3. Detailed Region Specifications

### 3.1 Block 0: Superblock

Block 0 holds the filesystem header. Filler serializes exactly 120 bytes of metadata starting at byte offset 0 of Block 0. The remaining 3,976 bytes of Block 0 are zeroed out.

```text
Block 0 (4096 bytes):
+--------------------+---------------------+----------------------+
| Header (120 bytes) | Unused / Zeroed Pad | 3,976 bytes          |
+--------------------+---------------------+----------------------+
0                    120                                          4096
```

#### On-Disk Superblock Field Offsets (`include/fs/Superblock.hpp`)

| Byte Offset | Field Name | Data Type | Size (Bytes) | Example / Default Value | Description |
|---:|---|---|---:|---|---|
| `0` | `magic` | `uint32_t` | 4 | `0x46494C4C` ("FILL") | Magic identifier validating Filler filesystem format |
| `4` | `version` | `uint32_t` | 4 | `1` | Filesystem version number |
| `8` | `block_size` | `uint32_t` | 4 | `4096` | Size of one allocation block in bytes |
| `12` | `inode_size` | `uint32_t` | 4 | `256` | Size of one on-disk inode in bytes |
| `16` | `total_blocks` | `uint64_t` | 8 | `16384` | Total number of blocks across the disk image |
| `24` | `total_inodes` | `uint64_t` | 8 | `256` | Total number of inodes supported in the inode table |
| `32` | `free_blocks` | `uint64_t` | 8 | `16364` (initially) | Dynamic count of unallocated blocks in the data region |
| `40` | `free_inodes` | `uint64_t` | 8 | `254` (initially) | Dynamic count of unallocated inodes (excluding 0 & 1) |
| `48` | `root_inode` | `uint64_t` | 8 | `1` | Inode number of the filesystem root directory (`/`) |
| `56` | `block_bitmap_start` | `uint64_t` | 8 | `1` | Starting block index of the block allocation bitmap |
| `64` | `block_bitmap_blocks`| `uint64_t` | 8 | `1` | Number of blocks occupied by the block bitmap |
| `72` | `inode_bitmap_start` | `uint64_t` | 8 | `2` | Starting block index of the inode allocation bitmap |
| `80` | `inode_bitmap_blocks`| `uint64_t` | 8 | `1` | Number of blocks occupied by the inode bitmap |
| `88` | `inode_table_start` | `uint64_t` | 8 | `3` | Starting block index of the inode table array |
| `96` | `inode_table_blocks` | `uint64_t` | 8 | `16` | Number of blocks occupied by the inode table |
| `104`| `data_region_start` | `uint64_t` | 8 | `19` | First allocatable block index of user data |
| `112`| `data_region_blocks` | `uint64_t` | 8 | `16365` | Total number of blocks in the user data region |
| **Total** | | | **120** | | `offsets::SERIALIZED_METADATA_SIZE` |

---

### 3.2 Block 1: Block Bitmap

The block bitmap tracks the allocation state of every block in the 64 MiB image (Blocks 0 through 16,383).
- **Bit convention**:
  - `0` = Block is **free** (available for allocation).
  - `1` = Block is **allocated** (reserved or containing data).
- **Capacity**: A 4096-byte block contains \(4096 \times 8 = 32,768\) bits. Since the disk has 16,384 blocks, only the first 16,384 bits (\(16,384 / 8 = 2048\) bytes) are used. The remaining 2048 bytes are set to 0.

#### Metadata Block Reservation in Bitmap
During formatting, the first 19 blocks (Blocks 0 through 18) contain system metadata and must never be allocated to user files. Thus:
- Blocks 0 to 18 are initialized to `1` in the block bitmap.
- In byte 0: Bits 0..7 correspond to Blocks 0..7 (value: `0xFF`).
- In byte 1: Bits 0..7 correspond to Blocks 8..15 (value: `0xFF`).
- In byte 2: Bits 0..2 correspond to Blocks 16, 17, 18 (value: `0x07`).
- All subsequent bits up to block 16,383 are initialized to `0` (free).
- When the root directory allocates its first data block (Block 19), Bit 3 of byte 2 is marked `1` (byte 2 becomes `0x0F`).

---

### 3.3 Block 2: Inode Bitmap

The inode bitmap tracks the allocation status of all 256 inodes (Inodes 0 through 255).
- **Capacity**: A 4096-byte block contains 32,768 bits. Filler uses only the first 256 bits (32 bytes). The trailing 4,064 bytes are zeroed.
- **Bit convention**:
  - `0` = Inode is **free**.
  - `1` = Inode is **allocated**.
- **Initial Formatted State**:
  - Inode 0 is permanently reserved (value `1` at bit 0).
  - Inode 1 is allocated to the root directory `/` (value `1` at bit 1).
  - Byte 0 is initialized to `0x03` (`0b00000011`).
  - Bytes 1 through 31 are initialized to `0x00` (Inodes 2..255 are free).

---

### 3.4 Blocks 3–18: Inode Table

The Inode Table is an array of 256 contiguous `Inode` structures. Each inode occupies 256 bytes.
- Total bytes = \(256 \times 256 = 65,536\) bytes.
- Total blocks = \(65,536 / 4096 = 16\) blocks (Blocks 3, 4, 5, ..., 18).
- Each 4096-byte block holds exactly 16 inodes:
  - Block 3: Inodes 0 – 15
  - Block 4: Inodes 16 – 31
  - Block 5: Inodes 32 – 47
  - ...
  - Block 18: Inodes 240 – 255

#### On-Disk Inode Structure (256 bytes, `include/fs/Inode.hpp`)

```text
+------------+-----------+-------------+-----------+-----------+------------+------------+
| inode_id   | type      | permissions | uid       | gid       | size_bytes | blocks_cnt |
| 8 bytes    | 4 bytes   | 4 bytes     | 4 bytes   | 4 bytes   | 8 bytes    | 8 bytes    |
+------------+-----------+-------------+-----------+-----------+------------+------------+
0            8           12            16          20          24           32           40

+------------+------------+------------+-------------------------------+
| atime_sec  | mtime_sec  | ctime_sec  | direct_blocks[16]             |
| 8 bytes    | 8 bytes    | 8 bytes    | 16 * 8 = 128 bytes            |
+------------+------------+------------+-------------------------------+
40           48           56           64                              192

+-----------------+-----------------+----------------------+
| single_indirect | double_indirect | reserved (pad)       |
| 8 bytes         | 8 bytes         | 48 bytes             |
+-----------------+-----------------+----------------------+
192               200               208                    256
```

#### Detailed Inode Field Map

| Byte Offset | Field Name | Data Type | Size (Bytes) | Description |
|---:|---|---|---:|---|
| `0` | `inode_id` | `uint64_t` | 8 | Inode identification number (0..255) |
| `8` | `type` | `uint32_t` | 4 | File type: `0` = UNKNOWN, `1` = REGULAR, `2` = DIRECTORY |
| `12` | `permissions` | `uint32_t` | 4 | Standard POSIX-style permissions (default: `0644` for files, `0755` for dirs) |
| `16` | `uid` | `uint32_t` | 4 | User ID (owner) |
| `20` | `gid` | `uint32_t` | 4 | Group ID |
| `24` | `size_bytes` | `uint64_t` | 8 | Logical file size in bytes |
| `32` | `blocks_count` | `uint64_t` | 8 | Total blocks allocated (data blocks + indirect pointer tables) |
| `40` | `atime_sec` | `int64_t` | 8 | Access time in seconds since Unix epoch |
| `48` | `mtime_sec` | `int64_t` | 8 | Modification time in seconds since Unix epoch |
| `56` | `ctime_sec` | `int64_t` | 8 | Inode status change time in seconds since Unix epoch |
| `64` | `direct_blocks[0..15]` | `uint64_t[16]` | 128 | 16 direct data block pointers (64 KiB total direct capacity) |
| `192` | `single_indirect` | `uint64_t` | 8 | Block pointer to a single-indirect pointer table |
| `200` | `double_indirect` | `uint64_t` | 8 | Block pointer to a double-indirect Level-1 pointer table |
| `208` | `reserved` | `uint8_t[48]` | 48 | Reserved padding ensuring exactly 256-byte alignment |
| **Total** | | | **256** | |

---

### 3.5 Blocks 19–16,383: Data Region

The data region starts at Block 19 and extends to the final block (Block 16,383).
- **Total blocks**: \(16,384 - 19 = 16,365\) blocks.
- **Total capacity**: \(16,365 \times 4096 = 67,031,040\) bytes (\(\approx 63.92\) MiB).
- **Content types stored in Data Region**:
  1. Regular file content (raw file bytes).
  2. Directory entry records (variable-length filename records).
  3. Single-indirect pointer tables (4096-byte blocks containing 512 `uint64_t` block pointers).
  4. Double-indirect Level-1 and Level-2 pointer tables.

---

## 4. Directory Entry On-Disk Format

Directories in Filler are stored like regular files whose contents are sequences of packed directory entry records. Filler packs directory entries with 4-byte record alignment.

#### Record Structure (`src/fs/DirectoryManager.cpp`)

```text
+-------------------+------------------+------------------+-----------------------+----------+
| inode_id          | type             | name_len         | name                  | padding  |
| uint64_t (8 B)    | uint32_t (4 B)   | uint32_t (4 B)   | (name_len bytes)      | (0..3 B) |
+-------------------+------------------+------------------+-----------------------+----------+
0                   8                  12                 16                      16+name_len
```

- **Header Size**: 16 bytes.
- **Record Length Calculation**:
  \[
  \text{rec\_len} = \left\lfloor \frac{16 + \text{name\_len} + 3}{4} \right\rfloor \times 4
  \]
- **Example**:
  - Entry name `"."` (length 1): \(16 + 1 = 17 \rightarrow \text{padded to } 20\) bytes.
  - Entry name `"hello.txt"` (length 9): \(16 + 9 = 25 \rightarrow \text{padded to } 28\) bytes.

---

## 5. Mathematical Mapping Formulas

### 5.1 Block ID to Physical Byte Offset

To locate any block on the disk image:
\[
\text{ByteOffset}(\text{block\_id}) = \text{block\_id} \times \text{BLOCK\_SIZE} = \text{block\_id} \times 4096
\]

**Example**:
- Block 0 (Superblock): \(0 \times 4096 = 0\)
- Block 1 (Block Bitmap): \(1 \times 4096 = 4096\) (`0x1000`)
- Block 3 (Start of Inode Table): \(3 \times 4096 = 12,288\) (`0x3000`)
- Block 19 (Start of Data Region): \(19 \times 4096 = 77,824\) (`0x13000`)

---

### 5.2 Inode Number to Disk Location

Given an `inode_id` in the range \(0 \le \text{inode\_id} < 256\):

1. **Calculate Inode Table Byte Offset**:
   \[
   \text{TableByteOffset}(\text{inode\_id}) = \text{inode\_id} \times \text{INODE\_SIZE} = \text{inode\_id} \times 256
   \]
2. **Calculate Inode Table Block Offset**:
   \[
   \text{RelativeBlock} = \left\lfloor \frac{\text{TableByteOffset}}{4096} \right\rfloor = \left\lfloor \frac{\text{inode\_id}}{16} \right\rfloor
   \]
   \[
   \text{AbsoluteBlock} = \text{inode\_table\_start} + \text{RelativeBlock} = 3 + \left\lfloor \frac{\text{inode\_id}}{16} \right\rfloor
   \]
3. **Calculate Offset Within the Block**:
   \[
   \text{BlockOffset} = (\text{inode\_id} \pmod{16}) \times 256
   \]

**Worked Example for Inode 1 (Root Inode)**:
- \(\text{RelativeBlock} = \lfloor 1 / 16 \rfloor = 0\)
- \(\text{AbsoluteBlock} = 3 + 0 = 3\)
- \(\text{BlockOffset} = (1 \pmod{16}) \times 256 = 256\)
- Physical byte offset = \(3 \times 4096 + 256 = 12,288 + 256 = 12,544\) (`0x3100`).

**Worked Example for Inode 35**:
- \(\text{RelativeBlock} = \lfloor 35 / 16 \rfloor = 2\)
- \(\text{AbsoluteBlock} = 3 + 2 = 5\)
- \(\text{BlockOffset} = (35 \pmod{16}) \times 256 = 3 \times 256 = 768\)
- Physical byte offset = \(5 \times 4096 + 768 = 20,480 + 768 = 21,248\) (`0x5300`).

---

### 5.3 Bitmap Bit and Byte Index Mapping

Given any resource ID (block number or inode number):
\[
\text{ByteIndex} = \left\lfloor \frac{\text{ID}}{8} \right\rfloor
\]
\[
\text{BitOffset} = \text{ID} \pmod{8}
\]
\[
\text{BitMask} = 1 \ll \text{BitOffset}
\]

**Checking if an item is allocated**:
\[
\text{IsAllocated} = (\text{BitmapData}[\text{ByteIndex}] \ \& \ \text{BitMask}) \ne 0
\]

**Allocating an item**:
\[
\text{BitmapData}[\text{ByteIndex}] = \text{BitmapData}[\text{ByteIndex}] \ | \ \text{BitMask}
\]

**Freeing an item**:
\[
\text{BitmapData}[\text{ByteIndex}] = \text{BitmapData}[\text{ByteIndex}] \ \& \ (\sim\text{BitMask})
\]

---

### 5.4 File Offset to Block Translation

Given a file offset \(O\) in bytes:

1. **Logical Block Index**:
   \[
   L = \left\lfloor \frac{O}{4096} \right\rfloor
   \]
2. **Offset Within Block**:
   \[
   B_{\text{offset}} = O \pmod{4096}
   \]
3. **Translation Hierarchy**:
   - **Case 1: Direct Blocks** (\(0 \le L < 16\)):
     - Physical block = `inode.direct_blocks[L]`
     - Capacity: \(16 \times 4096 = 65,536\) bytes (64 KiB).
   - **Case 2: Single Indirect Blocks** (\(16 \le L < 528\)):
     - Pointers per block = \(4096 / 8 = 512\).
     - Single-indirect relative index: \(I = L - 16\).
     - Read table at `inode.single_indirect`.
     - Physical block = `table[I]`.
     - Capacity: \(512 \times 4096 = 2,097,152\) bytes (2 MiB).
     - Cumulative capacity: \(65,536 + 2,097,152 = 2,162,688\) bytes (\(\approx 2.06\) MiB).
   - **Case 3: Double Indirect Blocks** (\(528 \le L < 262,672\)):
     - Double-indirect relative index: \(R = L - 528\).
     - Level-1 index: \(L_1 = \lfloor R / 512 \rfloor\).
     - Level-2 index: \(L_2 = R \pmod{512}\).
     - Read Level-1 table at `inode.double_indirect`.
     - Read Level-2 table at `Level1Table[L_1]`.
     - Physical block = `Level2Table[L_2]`.
     - Theoretical capacity: \(512 \times 512 \times 4096 = 1,073,741,824\) bytes (1 GiB).
     - Effective capacity in Filler: Bounded by the Data Region size (16,365 blocks, \(\approx 63.92\) MiB).

---

## 6. Endianness & Byte Serialization

Filler guarantees full portability across CPU architectures by enforcing strict **little-endian** serialization for all on-disk integers:
- `uint32_t`:
  - `dst[0] = val & 0xFF;`
  - `dst[1] = (val >> 8) & 0xFF;`
  - `dst[2] = (val >> 16) & 0xFF;`
  - `dst[3] = (val >> 24) & 0xFF;`
- `uint64_t`:
  - Serialized byte-by-byte for all 8 bytes (`val >> (i * 8)`).
- Never casts raw memory structs directly to or from disk buffers. This avoids compiler structure-packing discrepancies, padding anomalies, and big-endian CPU incompatibilities.

