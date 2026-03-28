#!/usr/bin/env python3
"""
AIOS ext2 Disk Image Creator

Usage: python3 tools/mkext2.py [output] [size_mb]
"""
import struct, sys, os, time

def mkext2(img="disk_ext2.img", size_mb=64):
    block_size = 1024
    disk_size = size_mb * 1024 * 1024
    total_blocks = disk_size // block_size
    blocks_per_group = 8192
    inodes_per_group = 256
    inode_size = 128
    num_groups = (total_blocks + blocks_per_group - 1) // blocks_per_group

    total_inodes = num_groups * inodes_per_group

    print(f"Creating {img} ({size_mb} MiB ext2)")
    print(f"  Block size: {block_size}, Total blocks: {total_blocks}")
    print(f"  Block groups: {num_groups}, Inodes/group: {inodes_per_group}")
    print(f"  Total inodes: {total_inodes}")

    data = bytearray(disk_size)
    now = int(time.time())

    # ── Superblock (block 1, byte offset 1024) ──
    sb = bytearray(1024)
    struct.pack_into('<I', sb, 0, total_inodes)           # s_inodes_count
    struct.pack_into('<I', sb, 4, total_blocks)            # s_blocks_count
    struct.pack_into('<I', sb, 8, 0)                       # s_r_blocks_count
    free_blocks = total_blocks  # will adjust below
    struct.pack_into('<I', sb, 16, 0)                      # s_free_inodes_count (set later)
    struct.pack_into('<I', sb, 20, 1)                      # s_first_data_block (1 for 1K blocks)
    struct.pack_into('<I', sb, 24, 0)                      # s_log_block_size (0 = 1024)
    struct.pack_into('<I', sb, 28, 0)                      # s_log_frag_size
    struct.pack_into('<I', sb, 32, blocks_per_group)       # s_blocks_per_group
    struct.pack_into('<I', sb, 36, blocks_per_group)       # s_frags_per_group
    struct.pack_into('<I', sb, 40, inodes_per_group)       # s_inodes_per_group
    struct.pack_into('<I', sb, 44, now)                    # s_mtime
    struct.pack_into('<I', sb, 48, now)                    # s_wtime
    struct.pack_into('<H', sb, 52, 0)                      # s_mnt_count
    struct.pack_into('<H', sb, 54, 20)                     # s_max_mnt_count
    struct.pack_into('<H', sb, 56, 0xEF53)                 # s_magic
    struct.pack_into('<H', sb, 58, 1)                      # s_state (clean)
    struct.pack_into('<H', sb, 60, 1)                      # s_errors (continue)
    struct.pack_into('<I', sb, 76, now)                     # s_lastcheck
    struct.pack_into('<I', sb, 88, inode_size)              # s_inode_size
    sb[120:136] = b'AIOS\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'  # s_volume_name

    # ── Block Group Descriptor Table (block 2) ──
    bgdt = bytearray(num_groups * 32)

    used_blocks_total = 0
    used_inodes_total = 0

    root_data_block = 0
    hello_inode = 11
    hello_data_block = 0

    for g in range(num_groups):
        group_start = 1 + g * blocks_per_group  # first data block is 1
        # Layout within each group:
        # Block 0 (of group): superblock copy (group 0) or data
        # Block 1: group descriptors
        # Block 2: block bitmap
        # Block 3: inode bitmap
        # Block 4..N: inode table
        # N+1..: data blocks

        inode_table_blocks = (inodes_per_group * inode_size + block_size - 1) // block_size

        if g == 0:
            bb = group_start + 1  # block bitmap at block 2 (abs)
            ib = group_start + 2  # inode bitmap at block 3
            it = group_start + 3  # inode table at block 4
        else:
            bb = group_start + 0
            ib = group_start + 1
            it = group_start + 2

        data_start = it + inode_table_blocks

        # Overhead blocks
        if g == 0:
            overhead = 1 + 1 + 1 + 1 + inode_table_blocks  # sb + bgdt + bb + ib + it
        else:
            overhead = 1 + 1 + inode_table_blocks  # bb + ib + it

        blocks_in_group = min(blocks_per_group, total_blocks - g * blocks_per_group)
        free_in_group = blocks_in_group - overhead
        free_inodes_in_group = inodes_per_group

        # Mark overhead blocks as used in block bitmap
        bb_data = bytearray(block_size)
        for b in range(overhead):
            bb_data[b // 8] |= (1 << (b % 8))

        # Group 0: also mark root dir data block
        if g == 0:
            root_data_block = data_start
            bb_data[overhead // 8] |= (1 << (overhead % 8))
            free_in_group -= 1

            # Mark inodes 1 (bad blocks) and 2 (root) as used
            ib_data = bytearray(block_size)
            ib_data[0] = 0x03  # inodes 1 and 2 used
            free_inodes_in_group -= 2
            data[ib * block_size : ib * block_size + block_size] = ib_data

            # Also mark hello.txt inode (inode 11) and its data block
            hello_data_block = data_start + 1
            ib_data[(hello_inode - 1) // 8] |= (1 << ((hello_inode - 1) % 8))
            bb_data[(overhead + 1) // 8] |= (1 << ((overhead + 1) % 8))
            free_inodes_in_group -= 1
            free_in_group -= 1
            data[ib * block_size : ib * block_size + block_size] = ib_data
        else:
            ib_data = bytearray(block_size)
            data[ib * block_size : ib * block_size + block_size] = ib_data

        data[bb * block_size : bb * block_size + block_size] = bb_data

        # Block group descriptor
        off = g * 32
        struct.pack_into('<I', bgdt, off + 0, bb)          # bg_block_bitmap
        struct.pack_into('<I', bgdt, off + 4, ib)          # bg_inode_bitmap
        struct.pack_into('<I', bgdt, off + 8, it)          # bg_inode_table
        struct.pack_into('<H', bgdt, off + 12, free_in_group)
        struct.pack_into('<H', bgdt, off + 14, free_inodes_in_group)
        struct.pack_into('<H', bgdt, off + 16, 0)          # bg_used_dirs_count

        used_blocks_total += overhead + (1 if g == 0 else 0) + (1 if g == 0 else 0)
        used_inodes_total += (2 if g == 0 else 0) + (1 if g == 0 else 0)

    free_blocks = total_blocks - used_blocks_total
    free_inodes = total_inodes - used_inodes_total
    struct.pack_into('<I', sb, 12, free_blocks)
    struct.pack_into('<I', sb, 16, free_inodes)

    # Write superblock at offset 1024
    data[1024:2048] = sb

    # Write BGDT at block 2 (offset 2048)
    data[2048:2048 + len(bgdt)] = bgdt

    # ── Root directory inode (inode 2) ──
    # Inode table for group 0 starts at block 4 (offset 4096)
    it0 = 4  # block number
    inode_off = it0 * block_size + (2 - 1) * inode_size  # inode 2, 0-indexed from 1

    root_inode = bytearray(inode_size)
    struct.pack_into('<H', root_inode, 0, 0o40755)        # i_mode (directory)
    struct.pack_into('<H', root_inode, 2, 0)               # i_uid
    struct.pack_into('<I', root_inode, 4, block_size)      # i_size (one block)
    struct.pack_into('<I', root_inode, 8, now)             # i_atime
    struct.pack_into('<I', root_inode, 12, now)            # i_ctime
    struct.pack_into('<I', root_inode, 16, now)            # i_mtime
    struct.pack_into('<H', root_inode, 26, 2)              # i_links_count
    struct.pack_into('<I', root_inode, 28, 2)              # i_blocks (in 512-byte units)
    struct.pack_into('<I', root_inode, 40, root_data_block) # i_block[0]

    data[inode_off:inode_off + inode_size] = root_inode

    # ── Root directory data ──
    root_dir = bytearray(block_size)
    pos = 0

    # "." entry
    struct.pack_into('<I', root_dir, pos, 2)        # inode 2
    struct.pack_into('<H', root_dir, pos+4, 12)     # rec_len
    root_dir[pos+6] = 1                              # name_len
    root_dir[pos+7] = 2                              # file_type (dir)
    root_dir[pos+8] = ord('.')
    pos += 12

    # ".." entry
    struct.pack_into('<I', root_dir, pos, 2)        # inode 2 (root parent = self)
    struct.pack_into('<H', root_dir, pos+4, 12)     # rec_len
    root_dir[pos+6] = 2                              # name_len
    root_dir[pos+7] = 2                              # file_type (dir)
    root_dir[pos+8] = ord('.')
    root_dir[pos+9] = ord('.')
    pos += 12

    # "hello.txt" entry
    hello_name = b'hello.txt'
    rec_len = block_size - pos  # last entry takes remaining space
    struct.pack_into('<I', root_dir, pos, hello_inode)
    struct.pack_into('<H', root_dir, pos+4, rec_len)
    root_dir[pos+6] = len(hello_name)
    root_dir[pos+7] = 1                              # file_type (regular)
    root_dir[pos+8:pos+8+len(hello_name)] = hello_name
    pos += rec_len

    data[root_data_block * block_size : root_data_block * block_size + block_size] = root_dir

    # ── hello.txt inode (inode 11) ──
    hello_content = b'Hello from AIOS ext2 filesystem!\n'
    hello_inode_off = it0 * block_size + (hello_inode - 1) * inode_size

    hi = bytearray(inode_size)
    struct.pack_into('<H', hi, 0, 0o100644)           # i_mode (regular file)
    struct.pack_into('<I', hi, 4, len(hello_content))  # i_size
    struct.pack_into('<I', hi, 8, now)                 # i_atime
    struct.pack_into('<I', hi, 12, now)                # i_ctime
    struct.pack_into('<I', hi, 16, now)                # i_mtime
    struct.pack_into('<H', hi, 26, 1)                  # i_links_count
    struct.pack_into('<I', hi, 28, 2)                  # i_blocks (512-byte units)
    struct.pack_into('<I', hi, 40, hello_data_block)   # i_block[0]

    data[hello_inode_off:hello_inode_off + inode_size] = hi

    # ── hello.txt data ──
    data[hello_data_block * block_size : hello_data_block * block_size + len(hello_content)] = hello_content

    # Write image
    with open(img, 'wb') as f:
        f.write(data)

    print(f"  Root dir at block {root_data_block}")
    print(f"  hello.txt: inode {hello_inode}, data block {hello_data_block}")
    print(f"  Content: {hello_content.decode()}", end='')
    print("Done.")

if __name__ == '__main__':
    output = sys.argv[1] if len(sys.argv) > 1 else 'disk_ext2.img'
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 64
    mkext2(output, size)
