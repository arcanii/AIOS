"""
AIOS ext2 Image Builder — multi-group, multi-block directories, indirect blocks.
"""
import struct, time, glob, os

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
        self.next_block_in_group = [0] * self.num_groups
        self.group_meta = []
        self.dirs = {}
        self.dir_blocks = {}

    def _write_block(self, block, bdata):
        off = block * self.block_size
        self.data[off:off+self.block_size] = bdata[:self.block_size]

    def _read_block(self, block):
        off = block * self.block_size
        return bytearray(self.data[off:off+self.block_size])

    def _alloc_inode(self):
        ino = self.next_inode
        self.next_inode += 1
        bb, ib, it, ds = self.group_meta[0]
        bmp = self._read_block(ib)
        idx = ino - 1
        bmp[idx // 8] |= (1 << (idx % 8))
        self._write_block(ib, bmp)
        return ino

    def _alloc_block(self):
        for g in range(self.num_groups):
            bb, ib, it, ds = self.group_meta[g]
            group_start = 1 + g * self.blocks_per_group if g == 0 else g * self.blocks_per_group
            max_in_group = min(self.blocks_per_group, self.total_blocks - g * self.blocks_per_group)
            bmp = self._read_block(bb)
            idx = self.next_block_in_group[g]
            while idx < max_in_group:
                if idx // 8 >= self.block_size:
                    break
                if not (bmp[idx // 8] & (1 << (idx % 8))):
                    bmp[idx // 8] |= (1 << (idx % 8))
                    self._write_block(bb, bmp)
                    self.next_block_in_group[g] = idx + 1
                    return group_start + idx
                idx += 1
            self.next_block_in_group[g] = idx
        raise RuntimeError("Out of blocks!")

    def _write_inode(self, ino, mode, size, blocks, links=1):
        bb, ib, it, ds = self.group_meta[0]
        off = it * self.block_size + (ino - 1) * self.inode_size
        inode = bytearray(self.inode_size)
        struct.pack_into('<H', inode, 0, mode)
        struct.pack_into('<I', inode, 4, size)
        struct.pack_into('<I', inode, 8, self.now)
        struct.pack_into('<I', inode, 12, self.now)
        struct.pack_into('<I', inode, 16, self.now)
        struct.pack_into('<H', inode, 26, links)
        struct.pack_into('<I', inode, 28, (size + 511) // 512)
        for i in range(min(12, len(blocks))):
            struct.pack_into('<I', inode, 40 + i * 4, blocks[i])
        if len(blocks) > 12:
            ptrs = self.block_size // 4
            ind_block = self._alloc_block()
            struct.pack_into('<I', inode, 40 + 12 * 4, ind_block)
            ind_data = bytearray(self.block_size)
            for i in range(12, min(len(blocks), 12 + ptrs)):
                struct.pack_into('<I', ind_data, (i - 12) * 4, blocks[i])
            self._write_block(ind_block, ind_data)
            if len(blocks) > 12 + ptrs:
                dind_block = self._alloc_block()
                struct.pack_into('<I', inode, 40 + 13 * 4, dind_block)
                dind_data = bytearray(self.block_size)
                remaining = blocks[12 + ptrs:]
                for gi in range((len(remaining) + ptrs - 1) // ptrs):
                    sub_ind = self._alloc_block()
                    struct.pack_into('<I', dind_data, gi * 4, sub_ind)
                    sub_data = bytearray(self.block_size)
                    for j in range(min(ptrs, len(remaining) - gi * ptrs)):
                        struct.pack_into('<I', sub_data, j * 4, remaining[gi * ptrs + j])
                    self._write_block(sub_ind, sub_data)
                self._write_block(dind_block, dind_data)
        self.data[off:off+self.inode_size] = inode

    def _write_dir_blocks(self, ino):
        entries = self.dirs.get(ino, [])
        if not entries:
            return
        block_entries = []
        blk = bytearray(self.block_size)
        pos = 0
        for i, (name, child_ino, ftype) in enumerate(entries):
            name_bytes = name.encode('ascii')
            name_len = len(name_bytes)
            rec_len = 8 + ((name_len + 3) & ~3)
            is_last = (i == len(entries) - 1)
            if pos + rec_len > self.block_size:
                if pos > 0 and pos < self.block_size:
                    scan = 0
                    prev = 0
                    while scan < pos:
                        prev = scan
                        scan += struct.unpack_from('<H', blk, scan + 4)[0]
                    struct.pack_into('<H', blk, prev + 4, self.block_size - prev)
                block_entries.append(bytes(blk))
                blk = bytearray(self.block_size)
                pos = 0
            if is_last:
                rec_len = self.block_size - pos
            struct.pack_into('<I', blk, pos, child_ino)
            struct.pack_into('<H', blk, pos + 4, rec_len)
            blk[pos + 6] = name_len
            blk[pos + 7] = ftype
            blk[pos + 8:pos + 8 + name_len] = name_bytes
            pos += rec_len
        block_entries.append(bytes(blk))
        if ino not in self.dir_blocks:
            self.dir_blocks[ino] = []
        while len(self.dir_blocks[ino]) < len(block_entries):
            self.dir_blocks[ino].append(self._alloc_block())
        for i, bdata in enumerate(block_entries):
            self._write_block(self.dir_blocks[ino][i], bdata)
        total_size = len(block_entries) * self.block_size
        self._write_inode(ino, 0o40755, total_size, self.dir_blocks[ino], links=2)

    def build_metadata(self):
        inode_table_blocks = (self.inodes_per_group * self.inode_size + self.block_size - 1) // self.block_size
        for g in range(self.num_groups):
            group_start = 1 + g * self.blocks_per_group if g == 0 else g * self.blocks_per_group
            if g == 0:
                bb, ib, it = group_start + 2, group_start + 3, group_start + 4
                overhead = 2 + 1 + 1 + inode_table_blocks
            else:
                bb, ib, it = group_start, group_start + 1, group_start + 2
                overhead = 1 + 1 + inode_table_blocks
            self.group_meta.append((bb, ib, it, it + inode_table_blocks))
            bmp = bytearray(self.block_size)
            for b in range(overhead):
                bmp[b // 8] |= (1 << (b % 8))
            self._write_block(bb, bmp)
            self.next_block_in_group[g] = overhead
            ibmp = bytearray(self.block_size)
            if g == 0:
                ibmp[0] = 0x03
            self._write_block(ib, ibmp)
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
        bgdt = bytearray(self.num_groups * 32)
        for g in range(self.num_groups):
            bb, ib, it, ds = self.group_meta[g]
            off = g * 32
            struct.pack_into('<I', bgdt, off, bb)
            struct.pack_into('<I', bgdt, off + 4, ib)
            struct.pack_into('<I', bgdt, off + 8, it)
        self.data[2048:2048 + len(bgdt)] = bgdt

    def mkdir(self, parent_ino, name):
        ino = self._alloc_inode()
        blk = self._alloc_block()
        self.dir_blocks[ino] = [blk]
        self.dirs[ino] = [('.', ino, 2), ('..', parent_ino, 2)]
        self._write_dir_blocks(ino)
        if parent_ino in self.dirs:
            self.dirs[parent_ino].append((name, ino, 2))
            self._write_dir_blocks(parent_ino)
        return ino

    def mkfile(self, parent_ino, name, content):
        ino = self._alloc_inode()
        data = content.encode('utf-8') if isinstance(content, str) else content
        size = len(data)
        num_blocks = max(1, (size + self.block_size - 1) // self.block_size)
        blocks = []
        for i in range(num_blocks):
            blk = self._alloc_block()
            blocks.append(blk)
            cs, ce = i * self.block_size, min((i + 1) * self.block_size, size)
            bd = bytearray(self.block_size)
            bd[:ce - cs] = data[cs:ce]
            self._write_block(blk, bd)
        self._write_inode(ino, 0o100755, size, blocks)
        if parent_ino in self.dirs:
            self.dirs[parent_ino].append((name, ino, 1))
            self._write_dir_blocks(parent_ino)
        return ino

    def install_rootfs(self, parent_ino, rootfs_dir, dir_inos=None):
        """Recursively install files from a host directory."""
        if dir_inos is None:
            dir_inos = {'/': parent_ino}
        for entry in sorted(os.listdir(rootfs_dir)):
            host_path = os.path.join(rootfs_dir, entry)
            if os.path.isdir(host_path):
                ino = self.mkdir(parent_ino, entry)
                dir_inos['/' + entry] = ino
                self.install_rootfs(ino, host_path, dir_inos)
            else:
                with open(host_path, 'rb') as f:
                    data = f.read()
                self.mkfile(parent_ino, entry, data)
                print(f'  /{entry} ({len(data)} bytes)')

    def install_elfs(self, bin_ino, elf_dirs):
        """Install ELF binaries from directories into /bin."""
        installed = 0
        seen = set()
        for elf_dir in elf_dirs:
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
                if name in seen:
                    continue
                seen.add(name)
                with open(elf_path, 'rb') as f:
                    elf_data = f.read()
                self.mkfile(bin_ino, name, elf_data)
                installed += 1
                print(f'  /bin/{name} ({len(elf_data)} bytes)')
        print(f'  Installed {installed} programs to /bin')

    def install_elfs_subdir(self, bin_ino, elf_dirs, subdir):
        """Install ELF binaries into /bin/<subdir>/."""
        sub_ino = self.mkdir(bin_ino, subdir)
        installed = 0
        seen = set()
        for elf_dir in elf_dirs:
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
                if name in seen:
                    continue
                seen.add(name)
                with open(elf_path, 'rb') as f:
                    elf_data = f.read()
                self.mkfile(sub_ino, name, elf_data)
                installed += 1
                print(f'  /bin/{subdir}/{name} ({len(elf_data)} bytes)')
        print(f'  Installed {installed} programs to /bin/{subdir}')

    def build(self, path, rootfs=None, elf_dirs=None, aios_dirs=None):
        self.build_metadata()

        root_blk = self._alloc_block()
        self.dir_blocks[2] = [root_blk]
        self.dirs[2] = [('.', 2, 2), ('..', 2, 2)]
        self._write_dir_blocks(2)

        # Standard directories
        bin_ino = self.mkdir(2, 'bin')
        self.mkdir(2, 'sbin')
        self.mkdir(2, 'home')
        self.mkdir(2, 'tmp')
        self.mkdir(2, 'dev')
        self.mkdir(2, 'var')
        self.mkdir(2, 'proc')

        # Install rootfs overlay (creates /etc, files, etc.)
        if rootfs and os.path.isdir(rootfs):
            print(f'Installing rootfs from {rootfs}:')
            self.install_rootfs(2, rootfs)

        # Install ELFs to /bin
        if elf_dirs:
            self.install_elfs(bin_ino, elf_dirs)

        # Install AIOS programs to /bin/aios
        if aios_dirs:
            self.install_elfs_subdir(bin_ino, aios_dirs, 'aios')

        with open(path, 'wb') as f:
            f.write(self.data)
        used = sum(self.next_block_in_group)
        print(f'Created {path} ({len(self.data) // (1024*1024)} MB)')
        print(f'  Inodes: {self.next_inode - 1}, Blocks: ~{used} across {self.num_groups} groups')
