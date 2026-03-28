#!/usr/bin/env python3
"""
Inject files into an AIOS ext2 disk image.
Usage: python3 tools/ext2_inject.py disk_ext2.img file1 [file2 ...]

Each file is added to the root directory with its basename uppercased.
"""
import struct, sys, os, glob

def rd16(data, off): return struct.unpack_from('<H', data, off)[0]
def rd32(data, off): return struct.unpack_from('<I', data, off)[0]
def wr16(data, off, v): struct.pack_into('<H', data, off, v)
def wr32(data, off, v): struct.pack_into('<I', data, off, v)

def inject(img_path, files):
    with open(img_path, 'rb') as f:
        data = bytearray(f.read())

    # Parse superblock at offset 1024
    sb_off = 1024
    total_inodes = rd32(data, sb_off + 0)
    total_blocks = rd32(data, sb_off + 4)
    first_data_block = rd32(data, sb_off + 20)
    log_block_size = rd32(data, sb_off + 24)
    block_size = 1024 << log_block_size
    blocks_per_group = rd32(data, sb_off + 32)
    inodes_per_group = rd32(data, sb_off + 40)
    inode_size = rd32(data, sb_off + 88)
    if inode_size == 0: inode_size = 128

    num_groups = (total_blocks + blocks_per_group - 1) // blocks_per_group

    print(f"ext2 image: {total_blocks} blocks, {block_size}B/block, "
          f"{num_groups} groups, {inodes_per_group} inodes/group, "
          f"inode_size={inode_size}")

    # Parse block group descriptor table (block after superblock)
    bgdt_off = (first_data_block + 1) * block_size

    def get_group_desc(g):
        off = bgdt_off + g * 32
        return {
            'block_bitmap': rd32(data, off + 0),
            'inode_bitmap': rd32(data, off + 4),
            'inode_table': rd32(data, off + 8),
            'free_blocks': rd16(data, off + 12),
            'free_inodes': rd16(data, off + 14),
        }

    def set_group_free_blocks(g, n):
        off = bgdt_off + g * 32
        wr16(data, off + 12, n)

    def set_group_free_inodes(g, n):
        off = bgdt_off + g * 32
        wr16(data, off + 14, n)

    def block_off(b):
        return b * block_size

    def inode_off(ino):
        """Get byte offset of inode number (1-based)"""
        g = (ino - 1) // inodes_per_group
        idx = (ino - 1) % inodes_per_group
        gd = get_group_desc(g)
        return gd['inode_table'] * block_size + idx * inode_size

    def alloc_inode(group=0):
        """Allocate next free inode in group"""
        gd = get_group_desc(group)
        bm_off = gd['inode_bitmap'] * block_size
        for i in range(inodes_per_group):
            byte_idx = i // 8
            bit_idx = i % 8
            if not (data[bm_off + byte_idx] & (1 << bit_idx)):
                data[bm_off + byte_idx] |= (1 << bit_idx)
                set_group_free_inodes(group, gd['free_inodes'] - 1)
                return group * inodes_per_group + i + 1  # 1-based
        return None

    def alloc_block(group=0):
        """Allocate next free block in group"""
        gd = get_group_desc(group)
        bm_off = gd['block_bitmap'] * block_size
        for i in range(blocks_per_group):
            byte_idx = i // 8
            bit_idx = i % 8
            if not (data[bm_off + byte_idx] & (1 << bit_idx)):
                data[bm_off + byte_idx] |= (1 << bit_idx)
                set_group_free_blocks(group, gd['free_blocks'] - 1)
                return group * blocks_per_group + first_data_block + i
        # Try other groups
        for g in range(num_groups):
            if g == group: continue
            gd2 = get_group_desc(g)
            bm_off2 = gd2['block_bitmap'] * block_size
            for i in range(blocks_per_group):
                byte_idx = i // 8
                bit_idx = i % 8
                if not (data[bm_off2 + byte_idx] & (1 << bit_idx)):
                    data[bm_off2 + byte_idx] |= (1 << bit_idx)
                    set_group_free_blocks(g, gd2['free_blocks'] - 1)
                    return g * blocks_per_group + first_data_block + i
        return None

    def alloc_blocks(count, group=0):
        blocks = []
        for _ in range(count):
            b = alloc_block(group)
            if b is None:
                print("ERROR: out of blocks!")
                sys.exit(1)
            blocks.append(b)
        return blocks

    def write_inode(ino, mode, size, blocks_list):
        """Write inode with direct block pointers"""
        off = inode_off(ino)
        import time
        now = int(time.time())
        inode = bytearray(inode_size)
        wr16(inode, 0, mode)       # i_mode
        wr32(inode, 4, size)       # i_size
        wr32(inode, 8, now)        # i_atime
        wr32(inode, 12, now)       # i_ctime
        wr32(inode, 16, now)       # i_mtime
        wr32(inode, 28, 1)         # i_links_count
        # i_blocks = number of 512-byte sectors
        total_sectors = len(blocks_list) * (block_size // 512)
        wr32(inode, 32, total_sectors)  # i_blocks (in 512-byte units)
        # Direct block pointers at offset 40
        for i, blk in enumerate(blocks_list[:12]):
            wr32(inode, 40 + i * 4, blk)
        data[off:off + inode_size] = inode

    def add_dir_entry(dir_ino, name, file_ino, file_type):
        """Add a directory entry to dir_ino's data blocks"""
        # Read inode to find data blocks
        d_off = inode_off(dir_ino)
        dir_size = rd32(data, d_off + 4)
        name_bytes = name.encode('ascii')
        name_len = len(name_bytes)
        # Entry size: 8 + name_len, rounded up to 4
        entry_size = ((8 + name_len) + 3) & ~3

        # Walk existing data blocks
        for bi in range(12):
            blk = rd32(data, d_off + 40 + bi * 4)
            if blk == 0:
                # Need a new block for this directory
                blk = alloc_block()
                if blk is None: return False
                wr32(data, d_off + 40 + bi * 4, blk)
                # Zero the block
                data[block_off(blk):block_off(blk) + block_size] = b'\x00' * block_size
                # Write entry at start
                eoff = block_off(blk)
                wr32(data, eoff + 0, file_ino)
                wr16(data, eoff + 4, block_size)  # rec_len = rest of block
                data[eoff + 6] = name_len
                data[eoff + 7] = file_type
                data[eoff + 8:eoff + 8 + name_len] = name_bytes
                # Update dir size
                dir_size += block_size
                wr32(data, d_off + 4, dir_size)
                # Update i_blocks
                cur_sectors = rd32(data, d_off + 32)
                wr32(data, d_off + 32, cur_sectors + block_size // 512)
                return True

            # Scan existing entries in this block
            boff = block_off(blk)
            pos = 0
            while pos < block_size:
                e_ino = rd32(data, boff + pos)
                e_rec_len = rd16(data, boff + pos + 4)
                if e_rec_len == 0: break
                e_name_len = data[boff + pos + 6]

                actual_len = ((8 + e_name_len) + 3) & ~3
                if e_ino == 0:
                    actual_len = 0

                free_space = e_rec_len - actual_len
                if free_space >= entry_size:
                    # Split this entry
                    if e_ino != 0:
                        wr16(data, boff + pos + 4, actual_len)
                        pos += actual_len
                        remaining = e_rec_len - actual_len
                    else:
                        remaining = e_rec_len

                    # Write new entry
                    wr32(data, boff + pos + 0, file_ino)
                    wr16(data, boff + pos + 4, remaining)
                    data[boff + pos + 6] = name_len
                    data[boff + pos + 7] = file_type
                    data[boff + pos + 8:boff + pos + 8 + name_len] = name_bytes
                    return True

                pos += e_rec_len
        return False

    # Root inode is always 2
    ROOT_INO = 2

    for filepath in files:
        file_data = open(filepath, 'rb').read()
        basename = os.path.basename(filepath)
        # Convert .bin -> .BIN with uppercase name
        name_parts = basename.rsplit('.', 1)
        if len(name_parts) == 2:
            disk_name = name_parts[0].upper() + '.' + name_parts[1].upper()
        else:
            disk_name = basename.upper()

        size = len(file_data)
        num_blocks = (size + block_size - 1) // block_size
        if num_blocks == 0: num_blocks = 1

        # Allocate inode and blocks
        ino = alloc_inode()
        if ino is None:
            print(f"  ERROR: no free inodes for {disk_name}")
            continue
        blocks = alloc_blocks(num_blocks)

        # Write file data to blocks
        for i, blk in enumerate(blocks):
            start = i * block_size
            end = min(start + block_size, size)
            chunk = file_data[start:end]
            data[block_off(blk):block_off(blk) + len(chunk)] = chunk

        # Write inode (mode 0100644 = regular file)
        write_inode(ino, 0o100644, size, blocks)

        # Add directory entry (file_type=1 for regular file)
        if add_dir_entry(ROOT_INO, disk_name, ino, 1):
            print(f"  {disk_name}: inode {ino}, {size} bytes, {num_blocks} blocks")
        else:
            print(f"  ERROR: could not add dir entry for {disk_name}")

    # Update superblock free counts
    total_free_inodes = 0
    total_free_blocks = 0
    for g in range(num_groups):
        gd = get_group_desc(g)
        total_free_inodes += gd['free_inodes']
        total_free_blocks += gd['free_blocks']
    wr32(data, sb_off + 12, total_free_blocks)
    wr32(data, sb_off + 16, total_free_inodes)

    with open(img_path, 'wb') as f:
        f.write(data)
    print(f"Injected {len(files)} files into {img_path}")

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: python3 tools/ext2_inject.py <image> <file1> [file2 ...]")
        sys.exit(1)
    inject(sys.argv[1], sys.argv[2:])
