#!/usr/bin/env python3
"""
AIOS ext2 Disk Image Creator (0.4.x)
Supports large files via indirect blocks.

Usage: python3 scripts/mkdisk.py [output] [size_mb]
       python3 scripts/mkdisk.py disk/disk_ext2.img 128 --install-elfs build-04/projects/aios/
"""
import struct, sys, os, time, glob

class Ext2Builder:
    def __init__(self, size_mb=128):
        self.block_size = 1024
        self.disk_size = size_mb * 1024 * 1024
        self.total_blocks = self.disk_size // self.block_size
        self.blocks_per_group = 8192
        self.inodes_per_group = 1024
        self.inode_size = 128
        self.num_groups = (self.total_blocks + self.blocks_per_group - 1) // self.blocks_per_group
        self.total_inodes = self.num_groups * self.inodes_per_group
        self.data = bytearray(self.disk_size)
        self.now = int(time.time())
        self.next_inode = 3
        self.next_data_block = 0
        self.it0 = 0
        self.inode_bmp_block = 0
        self.block_bmp_block = 0
        self.dirs = {}

    def _write_block(self, block, data):
        off = block * self.block_size
        self.data[off:off+len(data)] = data[:self.block_size]

    def _read_block(self, block):
        off = block * self.block_size
        return bytearray(self.data[off:off+self.block_size])

    def _alloc_inode(self):
        ino = self.next_inode
        self.next_inode += 1
        idx = ino - 1
        bmp = self._read_block(self.inode_bmp_block)
        bmp[idx // 8] |= (1 << (idx % 8))
        self._write_block(self.inode_bmp_block, bmp)
        return ino

    def _alloc_block(self):
        blk = self.next_data_block
        self.next_data_block += 1
        idx = blk - 1
        bmp = self._read_block(self.block_bmp_block)
        bmp[idx // 8] |= (1 << (idx % 8))
        self._write_block(self.block_bmp_block, bmp)
        return blk

    def _write_inode(self, ino, mode, size, blocks, links=1):
        """Write inode. blocks is list of block numbers (direct + indirect)."""
        off = self.it0 * self.block_size + (ino - 1) * self.inode_size
        inode = bytearray(self.inode_size)
        struct.pack_into('<H', inode, 0, mode)
        struct.pack_into('<I', inode, 4, size)
        struct.pack_into('<I', inode, 8, self.now)
        struct.pack_into('<I', inode, 12, self.now)
        struct.pack_into('<I', inode, 16, self.now)
        struct.pack_into('<H', inode, 26, links)
        struct.pack_into('<I', inode, 28, (size + 511) // 512)

        # Direct blocks (0-11)
        for i in range(min(12, len(blocks))):
            struct.pack_into('<I', inode, 40 + i * 4, blocks[i])

        # Single indirect (i_block[12])
        if len(blocks) > 12:
            ind_block = self._alloc_block()
            struct.pack_into('<I', inode, 40 + 12 * 4, ind_block)
            ind_data = bytearray(self.block_size)
            for i in range(12, min(len(blocks), 12 + self.block_size // 4)):
                struct.pack_into('<I', ind_data, (i - 12) * 4, blocks[i])
            self._write_block(ind_block, ind_data)

        self.data[off:off+self.inode_size] = inode

    def _write_dir_block(self, ino):
        entries = self.dirs.get(ino, [])
        blk_data = bytearray(self.block_size)
        pos = 0
        for i, (name, child_ino, ftype) in enumerate(entries):
            name_bytes = name.encode('ascii')
            name_len = len(name_bytes)
            rec_len = 8 + ((name_len + 3) & ~3)
            if i == len(entries) - 1:
                rec_len = self.block_size - pos
            struct.pack_into('<I', blk_data, pos, child_ino)
            struct.pack_into('<H', blk_data, pos + 4, rec_len)
            blk_data[pos + 6] = name_len
            blk_data[pos + 7] = ftype
            blk_data[pos + 8:pos + 8 + name_len] = name_bytes
            pos += rec_len
        ioff = self.it0 * self.block_size + (ino - 1) * self.inode_size
        blk = struct.unpack_from('<I', self.data, ioff + 40)[0]
        self._write_block(blk, blk_data)

    def build_metadata(self):
        bs = self.block_size
        inode_table_blocks = (self.inodes_per_group * self.inode_size + bs - 1) // bs
        self.block_bmp_block = 3
        self.inode_bmp_block = 4
        self.it0 = 5
        self.next_data_block = self.it0 + inode_table_blocks

        overhead = self.next_data_block - 1
        bmp = bytearray(bs)
        for b in range(overhead):
            bmp[b // 8] |= (1 << (b % 8))
        self._write_block(self.block_bmp_block, bmp)

        ibmp = bytearray(bs)
        ibmp[0] = 0x03
        self._write_block(self.inode_bmp_block, ibmp)

        sb = bytearray(1024)
        struct.pack_into('<I', sb, 0, self.total_inodes)
        struct.pack_into('<I', sb, 4, self.total_blocks)
        struct.pack_into('<I', sb, 20, 1)
        struct.pack_into('<I', sb, 24, 0)
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

        bgdt = bytearray(32)
        struct.pack_into('<I', bgdt, 0, self.block_bmp_block)
        struct.pack_into('<I', bgdt, 4, self.inode_bmp_block)
        struct.pack_into('<I', bgdt, 8, self.it0)
        self.data[2048:2048+32] = bgdt

    def mkdir(self, parent_ino, name):
        ino = self._alloc_inode()
        blk = self._alloc_block()
        self._write_inode(ino, 0o40755, self.block_size, [blk], links=2)
        self.dirs[ino] = [('.', ino, 2), ('..', parent_ino, 2)]
        self._write_dir_block(ino)
        if parent_ino in self.dirs:
            self.dirs[parent_ino].append((name, ino, 2))
            self._write_dir_block(parent_ino)
        return ino

    def mkfile(self, parent_ino, name, content):
        ino = self._alloc_inode()
        data = content.encode('utf-8') if isinstance(content, str) else content
        size = len(data)

        # Allocate blocks
        num_blocks = (size + self.block_size - 1) // self.block_size
        if num_blocks == 0:
            num_blocks = 1
        blocks = []
        for i in range(num_blocks):
            blk = self._alloc_block()
            blocks.append(blk)
            # Write data
            chunk_start = i * self.block_size
            chunk_end = min(chunk_start + self.block_size, size)
            blk_data = bytearray(self.block_size)
            blk_data[:chunk_end - chunk_start] = data[chunk_start:chunk_end]
            self._write_block(blk, blk_data)

        self._write_inode(ino, 0o100755, size, blocks)

        if parent_ino in self.dirs:
            self.dirs[parent_ino].append((name, ino, 1))
            self._write_dir_block(parent_ino)
        return ino

    def build(self, path, elf_dir=None):
        self.build_metadata()

        root_blk = self._alloc_block()
        self._write_inode(2, 0o40755, self.block_size, [root_blk], links=2)
        self.dirs[2] = [('.', 2, 2), ('..', 2, 2)]
        self._write_dir_block(2)

        bin_ino = self.mkdir(2, 'bin')
        self.mkdir(2, 'sbin')
        etc_ino = self.mkdir(2, 'etc')
        self.mkdir(2, 'home')
        self.mkdir(2, 'tmp')
        self.mkdir(2, 'dev')
        self.mkdir(2, 'var')
        self.mkdir(2, 'proc')

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

        # Install ELF binaries to /bin (original names, no stripping)
        if elf_dir:
            installed = 0
            for elf_path in sorted(glob.glob(os.path.join(elf_dir, '*'))):
                name = os.path.basename(elf_path)
                if name.startswith('CMake') or name.endswith('.o') or name.endswith('.a'):
                    continue
                if name in ('aios_root', 'apps_cpio.o'):
                    continue
                if os.path.isdir(elf_path):
                    continue
                with open(elf_path, 'rb') as f:
                    magic = f.read(4)
                if magic != b'\x7fELF':
                    continue
                with open(elf_path, 'rb') as f:
                    elf_data = f.read()
                self.mkfile(bin_ino, name, elf_data)
                installed += 1
                print(f'  /bin/{name} ({len(elf_data)} bytes)')
            print(f'  Installed {installed} programs to /bin')

        with open(path, 'wb') as f:
            f.write(self.data)

        print(f'Created {path} ({len(self.data) // (1024*1024)} MB)')
        print(f'  Inodes used: {self.next_inode - 1}')
        print(f'  Data blocks used: {self.next_data_block}')

if __name__ == '__main__':
    output = 'disk/disk_ext2.img'
    size = 128
    elf_dir = None

    args = sys.argv[1:]
    while args:
        if args[0] == '--install-elfs':
            elf_dir = args[1]
            args = args[2:]
        elif args[0].endswith('.img'):
            output = args[0]
            args = args[1:]
        else:
            try:
                size = int(args[0])
            except ValueError:
                pass
            args = args[1:]

    Ext2Builder(size).build(output, elf_dir)
