#!/usr/bin/env python3
"""ext2_ls.py — List contents of an ext2 disk image with directory tree."""
import struct
import sys

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <disk.img> [path]")
        sys.exit(1)

    with open(sys.argv[1], 'rb') as f:
        data = f.read()

    # Read superblock
    sb = data[1024:1024+256]
    block_size = 1024 << struct.unpack_from('<I', sb, 24)[0]
    inode_size = struct.unpack_from('<H', sb, 88)[0]
    total_inodes = struct.unpack_from('<I', sb, 0)[0]
    total_blocks = struct.unpack_from('<I', sb, 4)[0]
    free_inodes = struct.unpack_from('<I', sb, 16)[0]
    free_blocks = struct.unpack_from('<I', sb, 12)[0]

    # Read group descriptor to find inode table
    gd = data[block_size * 2:block_size * 2 + 32]
    inode_table_blk = struct.unpack_from('<I', gd, 8)[0]

    def read_inode(ino):
        off = block_size * inode_table_blk + (ino - 1) * inode_size
        return data[off:off + inode_size]

    def inode_blocks(idata):
        """Return list of data block numbers for an inode."""
        blocks = []
        for i in range(12):
            b = struct.unpack_from('<I', idata, 40 + i * 4)[0]
            if b: blocks.append(b)
        # Single indirect
        ind = struct.unpack_from('<I', idata, 40 + 12 * 4)[0]
        if ind:
            for j in range(block_size // 4):
                b = struct.unpack_from('<I', data, block_size * ind + j * 4)[0]
                if b: blocks.append(b)
        return blocks

    def list_dir(ino):
        """Return list of (name, inode, file_type) entries in a directory."""
        idata = read_inode(ino)
        size = struct.unpack_from('<I', idata, 4)[0]
        blks = inode_blocks(idata)
        entries = []
        remaining = size
        for blk in blks:
            off = block_size * blk
            pos = 0
            while pos < min(remaining, block_size):
                e_ino = struct.unpack_from('<I', data, off + pos)[0]
                rec_len = struct.unpack_from('<H', data, off + pos + 4)[0]
                if rec_len == 0:
                    break
                name_len = data[off + pos + 6]
                file_type = data[off + pos + 7]
                if e_ino != 0:
                    name = data[off + pos + 8:off + pos + 8 + name_len].decode('ascii', errors='replace')
                    entries.append((name, e_ino, file_type))
                pos += rec_len
            remaining -= block_size
        return entries

    def format_mode(mode):
        """Format inode mode as ls-style string."""
        s = 'd' if (mode & 0o170000) == 0o040000 else '-'
        for shift, chars in [(6, 'rwx'), (3, 'rwx'), (0, 'rwx')]:
            for i, c in enumerate(chars):
                s += c if mode & (1 << (shift + 2 - i)) else '-'
        return s

    def format_size(size):
        if size < 1024:
            return f"{size}B"
        elif size < 1024 * 1024:
            return f"{size/1024:.1f}K"
        else:
            return f"{size/(1024*1024):.1f}M"

    def print_tree(ino, prefix, indent=0):
        entries = list_dir(ino)
        dirs = []
        files = []
        for name, e_ino, ft in entries:
            if name in ('.', '..'):
                continue
            if ft == 2:
                dirs.append((name, e_ino))
            else:
                files.append((name, e_ino))

        # Sort
        dirs.sort()
        files.sort()

        for name, e_ino in dirs:
            idata = read_inode(e_ino)
            mode = struct.unpack_from('<H', idata, 0)[0]
            print(f"{'  ' * indent}{format_mode(mode)}  {name}/")
            print_tree(e_ino, prefix + name + "/", indent + 1)

        for name, e_ino in files:
            idata = read_inode(e_ino)
            mode = struct.unpack_from('<H', idata, 0)[0]
            size = struct.unpack_from('<I', idata, 4)[0]
            print(f"{'  ' * indent}{format_mode(mode)}  {format_size(size):>6s}  {name}")

    print(f"ext2 image: {sys.argv[1]}")
    print(f"  {total_blocks} blocks ({block_size}B), {total_inodes} inodes (size {inode_size})")
    print(f"  Free: {free_blocks} blocks, {free_inodes} inodes")
    print(f"")
    print(f"/")
    print_tree(2, "/")

if __name__ == '__main__':
    main()
