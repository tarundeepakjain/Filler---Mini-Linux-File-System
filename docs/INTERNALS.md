# Filler – Important Algorithms & Internal Workflows

This document details the core internal algorithms implemented across **Filler**. For each algorithm, we document its exact function signature, input parameters, return values, data structures, step-by-step logic (with pseudocode), edge case handling, failure conditions, and theoretical time complexity.

---

## 1. Disk Block Read and Write (`BlockDevice`)

- **Class**: `filler::storage::BlockDevice`
- **Signatures**:
  - `void read_block(uint64_t block_id, void* buffer, size_t buffer_size)`
  - `void write_block(uint64_t block_id, const void* buffer, size_t buffer_size)`

### Algorithm Steps:
```text
Algorithm: BlockDevice::read_block(block_id, buffer, buffer_size)
Input: block_id (uint64), buffer (pointer), buffer_size (size_t)
Output: None (buffer populated with 4096 bytes)

1. Check if device is open (fd_ >= 0). If not, throw std::runtime_error.
2. If buffer == nullptr, throw std::invalid_argument("Buffer pointer cannot be null").
3. If buffer_size < BLOCK_SIZE (4096), throw std::invalid_argument("Buffer size too small").
4. If block_id >= total_blocks_ (16384), throw std::out_of_range("Block ID out of range").
5. Calculate byte offset = block_id * BLOCK_SIZE.
6. Seek to offset using lseek(fd_, offset, SEEK_SET). If lseek < 0, throw std::system_error.
7. Execute full read loop:
     bytes_read = 0
     while bytes_read < BLOCK_SIZE:
         res = read(fd_, buffer + bytes_read, BLOCK_SIZE - bytes_read)
         if res < 0:
             if errno == EINTR: continue
             throw std::system_error(errno, "Read failed")
         if res == 0: throw std::runtime_error("Unexpected EOF while reading block")
         bytes_read += res
8. Return success.
```
- **Time Complexity**: \(O(1)\) (excluding physical disk spindle/NAND flash latency).

---

## 2. Filesystem Formatting (`Formatter`)

- **Class**: `filler::fs::Formatter`
- **Signature**: `Superblock format(uint64_t total_inodes = DEFAULT_TOTAL_INODES)`

### Algorithm Steps:
```text
Algorithm: Formatter::format(total_inodes)
Input: total_inodes (default: 256)
Output: Valid Superblock written to disk

1. Verify device_.total_blocks() == 16384 (64 MiB). If not, throw FormatterError.
2. Construct default Superblock layout via Superblock::create_default(16384, total_inodes):
     - magic = 0x46494C4C, version = 1, block_size = 4096, inode_size = 256
     - total_blocks = 16384, total_inodes = 256
     - block_bitmap_start = 1, block_bitmap_blocks = 1
     - inode_bitmap_start = 2, inode_bitmap_blocks = 1
     - inode_table_start = 3, inode_table_blocks = 16
     - data_region_start = 19, data_region_blocks = 16365
     - free_blocks = 16365, free_inodes = 254 (inodes 0 & 1 reserved)
3. Validate layout via sb.validate().
4. Write Superblock to Block 0 via sb.write(device_).
5. Initialize Block Bitmap (Block 1):
     - Allocate 4096-byte buffer filled with zeroes.
     - Mark metadata blocks [0..18] as allocated (set bits 0..18 to 1).
     - Write buffer to Block 1.
6. Initialize Inode Bitmap (Block 2):
     - Allocate 4096-byte buffer filled with zeroes.
     - Mark Inode 0 (reserved) and Inode 1 (root) as allocated (set bits 0 & 1 to 1; byte 0 = 0x03).
     - Write buffer to Block 2.
7. Zero-fill Inode Table (Blocks 3..18):
     - Write 16 blocks of zeroes to Blocks 3 through 18.
8. Initialize Root Directory:
     - DirectoryManager dm(device_);
     - dm.initialize_root(); (Creates Inode 1 with '.' and '..' pointing to 1).
9. Return created Superblock.
```
- **Time Complexity**: \(O(N)\) where \(N = 19\) metadata blocks written. Execution time \(\approx 1\) ms.

---

## 3. Superblock Loading and Validation (`Superblock`)

- **Class**: `filler::fs::Superblock`
- **Signature**: `static Superblock read(BlockDevice& device, bool check_device_size)`

### Algorithm Steps:
```text
Algorithm: Superblock::read(device, check_device_size)
1. Allocate 4096-byte buffer.
2. device.read_block(0, buffer.data(), 4096).
3. Deserialize 120 bytes using explicit little-endian unpacking:
     magic = read_u32_le(buffer + 0)
     version = read_u32_le(buffer + 4)
     block_size = read_u32_le(buffer + 8)
     ...
     data_region_blocks = read_u64_le(buffer + 112)
4. Validate invariants:
     - Check magic == 0x46494C4C. If not, throw SuperblockValidationError.
     - Check version == 1. If not, throw SuperblockValidationError.
     - Check block_size == 4096 and inode_size == 256.
     - Check contiguous region ordering:
         1 == block_bitmap_start
         2 == inode_bitmap_start
         3 == inode_table_start
         19 == data_region_start
     - Check total_blocks == data_region_start + data_region_blocks.
     - Check free_blocks <= data_region_blocks.
     - Check free_inodes <= total_inodes.
5. If check_device_size is true:
     - Ensure device.total_blocks() == sb.total_blocks.
6. Return validated Superblock.
```
- **Time Complexity**: \(O(1)\).

---

## 4. Block Allocation (`BlockBitmap`)

- **Class**: `filler::fs::BlockBitmap`
- **Signature**: `uint64_t allocate()`

### Algorithm Steps:
```text
Algorithm: BlockBitmap::allocate()
Output: Allocated physical block_id (uint64_t)

1. Check if sb_.free_blocks == 0. If so, throw NoFreeBlocksError.
2. Determine search start bit: start_bit = sb_.data_region_start (19).
3. Find first unset bit in bitmap data:
     start_byte = start_bit / 8
     for byte_idx from start_byte to (sb_.total_blocks / 8) - 1:
         if bitmap_data_[byte_idx] == 0xFF:
             continue // All 8 blocks in this byte are allocated
         for bit_idx from 0 to 7:
             candidate_block = (byte_idx * 8) + bit_idx
             if candidate_block < start_bit: continue
             if candidate_block >= sb_.total_blocks: break
             if (bitmap_data_[byte_idx] & (1 << bit_idx)) == 0:
                 // Found free block!
                 bitmap_data_[byte_idx] |= (1 << bit_idx) // Set bit to 1
                 sb_.free_blocks -= 1
                 // Persist to disk immediately
                 device_.write_block(sb_.block_bitmap_start, bitmap_data_.data(), 4096)
                 sb_.write(device_)
                 return candidate_block
4. If loop finishes without finding a bit, throw NoFreeBlocksError.
```
- **Time Complexity**: \(O(N)\) where \(N \le 2048\) bytes scanned (\(O(1)\) in practice for a 64 MiB disk).

---

## 5. Block Deallocation (`BlockBitmap`)

- **Class**: `filler::fs::BlockBitmap`
- **Signature**: `void free(uint64_t block_id)`

### Algorithm Steps:
```text
Algorithm: BlockBitmap::free(block_id)
Input: block_id (uint64_t)

1. If block_id < sb_.data_region_start (19) or block_id >= sb_.total_blocks (16384):
     throw BlockBitmapError("Cannot free out-of-range or metadata block").
2. byte_idx = block_id / 8
   bit_idx = block_id % 8
   bit_mask = (1 << bit_idx)
3. Check double-free:
     if (bitmap_data_[byte_idx] & bit_mask) == 0:
         throw BlockBitmapError("Block is already free (double-free detected)").
4. Clear bit:
     bitmap_data_[byte_idx] &= (~bit_mask)
5. Update Superblock counter:
     sb_.free_blocks += 1
6. Persist to disk:
     device_.write_block(sb_.block_bitmap_start, bitmap_data_.data(), 4096)
     sb_.write(device_)
```
- **Time Complexity**: \(O(1)\).

---

## 6. Inode Allocation (`InodeManager`)

- **Class**: `filler::fs::InodeManager`
- **Signature**: `uint64_t allocate_inode(FileType type, uint32_t permissions)`

### Algorithm Steps:
```text
Algorithm: InodeManager::allocate_inode(type, permissions)
1. Check if sb_.free_inodes == 0. If so, throw NoFreeInodesError.
2. Scan inode bitmap starting at Inode 2 (skipping 0 and 1):
     for ino from 2 to sb_.total_inodes - 1:
         byte_idx = ino / 8
         bit_idx = ino % 8
         if (inode_bitmap_data_[byte_idx] & (1 << bit_idx)) == 0:
             // Found free inode!
             inode_bitmap_data_[byte_idx] |= (1 << bit_idx)
             sb_.free_inodes -= 1
             
             // Construct fresh Inode
             Inode inode;
             inode.inode_id = ino
             inode.type = type
             inode.permissions = permissions
             inode.size_bytes = 0
             inode.blocks_count = 0
             now = current_unix_timestamp()
             inode.atime_sec = inode.mtime_sec = inode.ctime_sec = now
             
             // Persist Inode Table entry
             update_inode(ino, inode)
             // Persist Inode Bitmap & Superblock
             device_.write_block(sb_.inode_bitmap_start, inode_bitmap_data_.data(), 4096)
             sb_.write(device_)
             return ino
3. Throw NoFreeInodesError.
```
- **Time Complexity**: \(O(M)\) where \(M \le 32\) bytes scanned (\(O(1)\)).

---

## 7. Directory Entry Lookup (`DirectoryManager`)

- **Class**: `filler::fs::DirectoryManager`
- **Signature**: `uint64_t lookup(uint64_t parent_inode_id, const std::string& name)`

### Algorithm Steps:
```text
Algorithm: DirectoryManager::lookup(parent_inode_id, name)
1. Read parent Inode: parent = inode_manager_.get_inode(parent_inode_id).
2. If !parent.is_directory(), throw DirectoryManagerError("Not a directory").
3. Call read_entries(parent) to parse packed entries:
     For each entry in parsed entries:
         if entry.name == name:
             return entry.inode_id
4. Throw EntryNotFoundError(name).
```
- **Time Complexity**: \(O(K)\) where \(K\) is the number of entries in the directory (\(\le 16\) blocks scanned).

---

## 8. Directory Entry Insertion (`DirectoryManager`)

- **Class**: `filler::fs::DirectoryManager`
- **Signature**: `void add_entry(uint64_t parent_inode_id, uint64_t inode_id, FileType type, const std::string& name)`

### Algorithm Steps:
```text
Algorithm: DirectoryManager::add_entry(parent_inode_id, inode_id, type, name)
1. Validate name: cannot be empty, cannot contain '/', length <= 255.
2. Read parent Inode: parent = inode_manager_.get_inode(parent_inode_id).
3. Read current entries: entries = read_entries(parent).
4. Check for duplicates:
     for e in entries:
         if e.name == name: throw DirectoryManagerError("File or directory already exists").
5. Append new entry: entries.push_back({inode_id, type, name}).
6. Call write_entries(parent, entries):
     - Calculate packed byte size = sum of rec_len for all entries.
     - Calculate blocks_needed = ceil(total_size / 4096).
     - If blocks_needed > 16 (Inode::DIRECT_BLOCKS_COUNT):
         throw DirectoryManagerError("Directory size exceeds direct blocks capacity").
     - Allocate additional blocks via block_bitmap_.allocate() if blocks_needed > parent.blocks_count.
     - Free excess blocks if directory shrunk.
     - Pack and write 4096-byte chunks to parent.direct_blocks[0..blocks_needed-1].
     - Update parent.size_bytes = total_size.
     - Update parent.mtime_sec = now.
7. inode_manager_.update_inode(parent_inode_id, parent).
```
- **Time Complexity**: \(O(K)\) where \(K\) is the count of directory entries.

---

## 9. Path Resolution (`Shell`)

- **Class**: `filler::cli::Shell`
- **Signature**: `uint64_t resolve_path(const std::string& path, std::string* canonical_out)`

### Algorithm Steps:
```text
Algorithm: Shell::resolve_path(path, canonical_out)
Input: path string (relative or absolute), optional pointer to canonical_out string
Output: target inode_id (uint64_t)

1. If path is empty, return current_dir_inode_.
2. Determine start inode and base components:
     if path starts with '/':
         curr_inode = 1 (Root Inode)
         components = []
     else:
         curr_inode = current_dir_inode_
         components = tokenize(current_path_, '/')
3. Split input path by '/' into tokens.
4. For each token in tokens:
     if token == "" or token == ".":
         continue
     else if token == "..":
         curr_inode = directory_manager_->lookup(curr_inode, "..")
         if not components.empty(): components.pop_back()
     else:
         curr_inode = directory_manager_->lookup(curr_inode, token)
         components.push_back(token)
5. If canonical_out != nullptr:
     *canonical_out = "/" + join(components, "/")
6. Return curr_inode.
```
- **Time Complexity**: \(O(D \times K)\) where \(D\) is path depth and \(K\) is directory entry count.

---

## 10. File Writing (`FileManager`)

- **Class**: `filler::fs::FileManager`
- **Signature**: `size_t write(uint64_t inode_id, uint64_t offset, const void* data, size_t size)`

### Algorithm Steps:
```text
Algorithm: FileManager::write(inode_id, offset, data, size)
1. Inode inode = inode_manager_.get_inode(inode_id).
2. If inode.is_directory(), throw FileManagerError("Cannot write to directory").
3. bytes_written = 0
4. while bytes_written < size:
     curr_offset = offset + bytes_written
     logical_block = curr_offset / 4096
     block_offset = curr_offset % 4096
     chunk = min(size - bytes_written, 4096 - block_offset)
     
     // Translate logical block to physical block (allocating if not present)
     phys_block = get_or_allocate_block(inode, logical_block, allocate = true)
     
     // Read-Modify-Write if partial block write
     if chunk < 4096:
         device_.read_block(phys_block, block_buf, 4096)
     memcpy(block_buf + block_offset, data + bytes_written, chunk)
     device_.write_block(phys_block, block_buf, 4096)
     
     bytes_written += chunk
5. If offset + size > inode.size_bytes:
     inode.size_bytes = offset + size
6. inode.mtime_sec = now
7. inode_manager_.update_inode(inode_id, inode)
8. Return bytes_written.
```
- **Time Complexity**: \(O(B)\) where \(B = \lceil \text{size} / 4096 \rceil\) blocks.

---

## 11. File Reading (`FileManager`)

- **Class**: `filler::fs::FileManager`
- **Signature**: `size_t read(uint64_t inode_id, uint64_t offset, void* buffer, size_t size)`

### Algorithm Steps:
```text
Algorithm: FileManager::read(inode_id, offset, buffer, size)
1. Inode inode = inode_manager_.get_inode(inode_id).
2. If offset >= inode.size_bytes: return 0 (EOF reached).
3. to_read = min(size, inode.size_bytes - offset).
4. bytes_read = 0
5. while bytes_read < to_read:
     curr_offset = offset + bytes_read
     logical_block = curr_offset / 4096
     block_offset = curr_offset % 4096
     chunk = min(to_read - bytes_read, 4096 - block_offset)
     
     phys_block = get_or_allocate_block(inode, logical_block, allocate = false)
     if phys_block == 0:
         // Sparse gap: fill with zero bytes
         memset(buffer + bytes_read, 0, chunk)
     else:
         device_.read_block(phys_block, block_buf, 4096)
         memcpy(buffer + bytes_read, block_buf + block_offset, chunk)
     
     bytes_read += chunk
6. inode.atime_sec = now
7. inode_manager_.update_inode(inode_id, inode)
8. Return bytes_read.
```
- **Time Complexity**: \(O(B)\) where \(B\) is number of blocks read.

---

## 12. File Truncation with Rollback (`FileManager`)

- **Class**: `filler::fs::FileManager`
- **Signature**: `void truncate(uint64_t inode_id, uint64_t new_size)`

### Algorithm Steps:
```text
Algorithm: FileManager::truncate(inode_id, new_size)
1. Inode inode = inode_manager_.get_inode(inode_id).
2. If new_size == inode.size_bytes: return.

3. CASE 1: SHRINKING FILE (new_size < inode.size_bytes)
     old_blocks = ceil(inode.size_bytes / 4096)
     new_blocks = ceil(new_size / 4096)
     // Free all completely unused trailing blocks
     for blk from new_blocks to old_blocks - 1:
         free_logical_block(inode, blk)
     // Zero out partial trailing block slack space
     if new_size % 4096 != 0:
         last_blk = new_size / 4096
         phys = get_or_allocate_block(inode, last_blk, allocate = false)
         if phys != 0:
             device_.read_block(phys, buf, 4096)
             memset(buf + (new_size % 4096), 0, 4096 - (new_size % 4096))
             device_.write_block(phys, buf, 4096)
     inode.size_bytes = new_size
     inode.mtime_sec = inode.ctime_sec = now
     inode_manager_.update_inode(inode_id, inode)
     return

4. CASE 2: GROWING FILE (new_size > inode.size_bytes)
     working_inode = inode
     TruncateRollbackContext rollback_ctx;
     try:
         old_blocks = ceil(working_inode.size_bytes / 4096)
         new_blocks = ceil(new_size / 4096)
         for blk from old_blocks to new_blocks - 1:
             get_or_allocate_block(working_inode, blk, allocate = true, &rollback_ctx)
         working_inode.size_bytes = new_size
         working_inode.mtime_sec = working_inode.ctime_sec = now
         inode_manager_.update_inode(inode_id, working_inode)
     catch (...):
         // ROLLBACK:
         // 1. Restore all modified existing indirect pointer tables from disk backup
         for (blk_id, original_data) in rollback_ctx.existing_block_backups:
             device_.write_block(blk_id, original_data, 4096)
         // 2. Free all newly allocated blocks in reverse order
         for blk in reverse(rollback_ctx.newly_allocated_blocks):
             block_bitmap_.free(blk)
         // Inode on disk was never updated, so file size and pointers remain intact
         rethrow exception
```
- **Time Complexity**: \(O(B)\) where \(B\) is the number of blocks added or removed.

---

## 13. File Deletion with Safe Ordering (`FileManager`)

- **Class**: `filler::fs::FileManager`
- **Signature**: `void remove(uint64_t parent_inode_id, const std::string& filename)`

### Algorithm Steps:
```text
Algorithm: FileManager::remove(parent_inode_id, filename)
1. Validate inputs (filename not empty, not "." or "..").
2. Validate parent inode is allocated and is a directory.
3. Lookup file: file_inode_id = directory_manager_.lookup(parent_inode_id, filename).
4. Validate target inode: ensure it is a REGULAR file, not a directory.
5. STEP 1 (UNLINK FIRST):
     directory_manager_.remove_entry(parent_inode_id, filename)
     // If this step fails, execution halts. Inode and blocks remain 100% intact.
6. STEP 2 (FREE BLOCKS):
     Inode file_inode = inode_manager_.get_inode(file_inode_id)
     free_all_blocks(file_inode):
         - Free direct blocks 0..15
         - If single_indirect != 0:
             Read single-indirect table, free all referenced data blocks, free table block
         - If double_indirect != 0:
             Read L1 table; for each L2 table:
                 Read L2 table, free all referenced data blocks, free L2 table block
             Free L1 table block
7. STEP 3 (FREE INODE):
     inode_manager_.free_inode(file_inode_id)
```
- **Time Complexity**: \(O(B + K)\) where \(B\) is the number of blocks in the file and \(K\) is directory entry count.

---

## 14. Command Execution Dispatcher (`Shell`)

- **Class**: `filler::cli::Shell`
- **Signature**: `bool execute_command(const std::string& cmd_line, std::ostream& out)`

### Algorithm Steps:
```text
Algorithm: Shell::execute_command(cmd_line, out)
1. Trim leading and trailing whitespace.
2. If empty line: return true.
3. Extract first token as 'cmd'; remainder as 'args'.
4. If cmd == "exit": return false.
5. If cmd == "help": cmd_help(out); return true.
6. If cmd == "format": cmd_format(out); return true.
7. If filesystem is not formatted (!is_formatted_):
     if fs_state_ == UNFORMATTED:
         out << "Error: Filesystem is not formatted. Run 'format' first.\n"
     else:
         out << "Error: " << status_message_ << "\n"
     return true.
8. Dispatch based on cmd:
     "pwd"      -> cmd_pwd(out)
     "cd"       -> cmd_cd(args, out)
     "ls"       -> cmd_ls(args, out)
     "mkdir"    -> cmd_mkdir(args, out)
     "touch"    -> cmd_touch(args, out)
     "cat"      -> cmd_cat(args, out)
     "write"    -> cmd_write(args, out)
     "append"   -> cmd_append(args, out)
     "truncate" -> cmd_truncate(args, out)
     "rm"       -> cmd_rm(args, out)
     "rmdir"    -> cmd_rmdir(args, out)
     "stat"     -> cmd_stat(args, out)
     default    -> out << "Unknown command: " << cmd << ". Type 'help' for available commands.\n"
9. Catch std::exception and report error message.
10. Return true.
```
- **Time Complexity**: \(O(1)\) command parsing dispatch.

