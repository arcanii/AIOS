#!/usr/bin/env python3
"""
AIOS ext2 Disk Image Creator (0.4.x)
Creates a clean ext2 filesystem with proper directory structure.

Usage: python3 scripts/mkdisk.py [output] [size_mb]
"""
import struct, sys, os, time

class Ext2Builder:
    def __init__(self, size_mb=128):
        self.block_size = 1024
        self.disk_size = size_mb * 1024 * 1024
        self.total_blocks = self.disk_size // self.block_size
        self.blocks_per_group = 8192
        self.inodes_per_group = 512
        self.inode_size = 128
        self.num_groups = (self.total_blocks + self.blocks_per_group - 1) // self.blocks_per_group
        self.total_inodes = self.num_groups * self.inodes_per_group
        self.data = bytearray(self.disk_size)
        self.now = int(time.time())
        self.next_inode = 3  # 1=bad blocks, 2=root
        self.next_data_block = 0  # set after metadata
        self.it0 = 0  # inode table block for group 0
        self.inode_bmp_block = 0
        self.block_bmp_block = 0
        self.dirs = {}  # inode -> [(name, inode, type)]

    def _write_block(self, block, data):
        off = block * self.block_size
        self.data[off:off+len(data)] = data

    def _read_block(self, block):
        off = block * self.block_size
        return self.data[off:off+self.block_size]

    def _alloc_inode(self):
        ino = self.next_inode
        self.next_inode += 1
        # Mark in inode bitmap
        idx = ino - 1
        bmp = self._read_block(self.inode_bmp_block)
        bmp = bytearray(bmp)
        bmp[idx // 8] |= (1 << (idx % 8))
        self._write_block(self.inode_bmp_block, bmp)
        return ino

    def _alloc_block(self):
        blk = self.next_data_block
        self.next_data_block += 1
        # Mark in block bitmap
        idx = blk - 1  # blocks are 1-indexed for group 0
        bmp = self._read_block(self.block_bmp_block)
        bmp = bytearray(bmp)
        bmp[idx // 8] |= (1 << (idx % 8))
        self._write_block(self.block_bmp_block, bmp)
        return blk

    def _write_inode(self, ino, mode, size, block0, links=1):
        off = self.it0 * self.block_size + (ino - 1) * self.inode_size
        inode = bytearray(self.inode_size)
        struct.pack_into('<H', inode, 0, mode)
        struct.pack_into('<I', inode, 4, size)
        struct.pack_into('<I', inode, 8, self.now)
        struct.pack_into('<I', inode, 12, self.now)
        struct.pack_into('<I', inode, 16, self.now)
        struct.pack_into('<H', inode, 26, links)
        struct.pack_into('<I', inode, 28, (size + 511) // 512)
        struct.pack_into('<I', inode, 40, block0)
        self.data[off:off+self.inode_size] = inode

    def _write_dir_block(self, ino):
        entries = self.dirs.get(ino, [])
        blk_data = bytearray(self.block_size)
        pos = 0
        for i, (name, child_ino, ftype) in enumerate(entries):
            name_bytes = name.encode('ascii')
            name_len = len(name_bytes)
            # rec_len: 8 + name rounded up to 4
            rec_len = 8 + ((name_len + 3) & ~3)
            if i == len(entries) - 1:
                rec_len = self.block_size - pos  # last entry fills block
            struct.pack_into('<I', blk_data, pos, child_ino)
            struct.pack_into('<H', blk_data, pos + 4, rec_len)
            blk_data[pos + 6] = name_len
            blk_data[pos + 7] = ftype
            blk_data[pos + 8:pos + 8 + name_len] = name_bytes
            pos += rec_len
        # Find which block this dir inode points to
        ioff = self.it0 * self.block_size + (ino - 1) * self.inode_size
        blk = struct.unpack_from('<I', self.data, ioff + 40)[0]
        self._write_block(blk, blk_data)

    def build_metadata(self):
        bs = self.block_size
        inode_table_blocks = (self.inodes_per_group * self.inode_size + bs - 1) // bs

        # Group 0 layout: block1=sb, block2=bgdt, block3=bbmp, block4=ibmp, block5..=itable
        self.block_bmp_block = 3
        self.inode_bmp_block = 4
        self.it0 = 5
        self.next_data_block = self.it0 + inode_table_blocks

        # Mark metadata blocks in block bitmap
        overhead = self.next_data_block - 1  # blocks 1..next_data_block-1
        bmp = bytearray(bs)
        for b in range(overhead):
            bmp[b // 8] |= (1 << (b % 8))
        self._write_block(self.block_bmp_block, bmp)

        # Mark inodes 1,2 in inode bitmap
        ibmp = bytearray(bs)
        ibmp[0] = 0x03
        self._write_block(self.inode_bmp_block, ibmp)

        # Superblock
        sb = bytearray(1024)
        struct.pack_into('<I', sb, 0, self.total_inodes)
        struct.pack_into('<I', sb, 4, self.total_blocks)
        struct.pack_into('<I', sb, 20, 1)  # s_first_data_block
        struct.pack_into('<I', sb, 24, 0)  # s_log_block_size (0=1024)
        struct.pack_into('<I', sb, 32, self.blocks_per_group)
        struct.pack_into('<I', sb, 36, self.blocks_per_group)
        struct.pack_into('<I', sb, 40, self.inodes_per_group)
        struct.pack_into('<I', sb, 44, self.now)
        struct.pack_into('<I', sb, 48, self.now)
        struct.pack_into('<H', sb, 56, 0xEF53)
        struct.pack_into('<H', sb, 58, 1)
        struct.pack_into('<I', sb, 88, self.inode_size)
        sb[120:124] = b'AIOS'
        self.data[1024:2048] = sb

        # Group descriptor
        bgdt = bytearray(32)
        struct.pack_into('<I', bgdt, 0, self.block_bmp_block)
        struct.pack_into('<I', bgdt, 4, self.inode_bmp_block)
        struct.pack_into('<I', bgdt, 8, self.it0)
        self.data[2048:2048+32] = bgdt

    def mkdir(self, parent_ino, name):
        ino = self._alloc_inode()
        blk = self._alloc_block()
        self._write_inode(ino, 0o40755, self.block_size, blk, links=2)
        # Add . and .. to new dir
        self.dirs[ino] = [('.', ino, 2), ('..', parent_ino, 2)]
        self._write_dir_block(ino)
        # Add to parent
        if parent_ino in self.dirs:
            self.dirs[parent_ino].append((name, ino, 2))
            self._write_dir_block(parent_ino)
        return ino

    def mkfile(self, parent_ino, name, content):
        ino = self._alloc_inode()
        data = content.encode('utf-8') if isinstance(content, str) else content
        blk = self._alloc_block()
        # Write file data
        fdata = bytearray(self.block_size)
        fdata[:len(data)] = data
        self._write_block(blk, fdata)
        self._write_inode(ino, 0o100644, len(data), blk)
        # Add to parent
        if parent_ino in self.dirs:
            self.dirs[parent_ino].append((name, ino, 1))
            self._write_dir_block(parent_ino)
        return ino

    def build(self, path):
        self.build_metadata()

        # Root directory (inode 2)
        root_blk = self._alloc_block()
        self._write_inode(2, 0o40755, self.block_size, root_blk, links=2)
        self.dirs[2] = [('.', 2, 2), ('..', 2, 2)]
        self._write_dir_block(2)

        # Create directory structure
        bin_ino = self.mkdir(2, 'bin')
        sbin_ino = self.mkdir(2, 'sbin')
        etc_ino = self.mkdir(2, 'etc')
        home_ino = self.mkdir(2, 'home')
        tmp_ino = self.mkdir(2, 'tmp')
        dev_ino = self.mkdir(2, 'dev')
        var_ino = self.mkdir(2, 'var')
        proc_ino = self.mkdir(2, 'proc')

        # Create files
        self.mkfile(2, 'hello.txt', 'Hello from AIOS ext2 filesystem!\n')

        self.mkfile(etc_ino, 'hostname', 'aios\n')
        self.mkfile(etc_ino, 'motd',
            '  ___                      _         _\n'
            ' / _ \\ _ __   ___ _ __    / \\   _ __(_) ___  ___\n'
            '| | | | \'_ \\ / _ \\ \'_ \\  / _ \\ | \'__| |/ _ \\/ __|\n'
            '| |_| | |_) |  __/ | | |/ ___ \\| |  | |  __/\\__ \\\\\n'
            ' \\___/| .__/ \\___|_| |_/_/   \\_\\_|  |_|\\___||___/\n'
            '      |_|\n'
            '\n'
            '  Kernel:  seL4 15.0.0\n'
            '  Arch:    AArch64 (Cortex-A53, 4 cores SMP)\n'
            '  FS:      ext2 (virtio-blk)\n')
        self.mkfile(etc_ino, 'passwd', 'root:x:0:0:root:/root:/bin/sh\n')
        self.mkfile(etc_ino, 'fstab', '/dev/vda  /     ext2  defaults  0 1\nproc      /proc proc  defaults  0 0\n')
        self.mkfile(etc_ino, 'services.conf', '# AIOS services\nserial_server=enabled\nfs_server=enabled\nexec_server=enabled\n')

        with open(path, 'wb') as f:
            f.write(self.data)

        print(f'Created {path} ({len(self.data) // (1024*1024)} MB)')
        print(f'  Inodes used: {self.next_inode - 1}')
        print(f'  Data blocks used: {self.next_data_block}')

if __name__ == '__main__':
    output = sys.argv[1] if len(sys.argv) > 1 else 'disk/disk_ext2.img'
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 128
    Ext2Builder(size).build(output)
