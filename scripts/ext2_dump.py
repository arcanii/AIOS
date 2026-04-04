#!/usr/bin/env python3
"""
AIOS ext2 Image Inspector — dump directory structure and metadata.

Usage: python3 scripts/ext2_dump.py [disk.img] [--dir /path] [--all]
"""
import struct, sys, os

def rd16(b, o): return struct.unpack_from('<H', b, o)[0]
def rd32(b, o): return struct.unpack_from('<I', b, o)[0]

class Ext2Inspector:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.img = f.read()
        self.sb = self.img[1024:2048]
        self.magic = rd16(self.sb, 56)
        if self.magic != 0xEF53:
            raise ValueError(f"Not ext2: magic=0x{self.magic:04X}")
        self.bs = 1024 << rd32(self.sb, 24)
        self.total_inodes = rd32(self.sb, 0)
        self.total_blocks = rd32(self.sb, 4)
        self.inodes_per_group = rd32(self.sb, 40)
        self.inode_size = rd32(self.sb, 88) or 128
        self.blocks_per_group = rd32(self.sb, 32)
        self.num_groups = (self.total_blocks + self.blocks_per_group - 1) // self.blocks_per_group
        self.bgdt = self.img[2048:2048 + self.num_groups * 32]

    def block(self, n):
        return self.img[n * self.bs:(n + 1) * self.bs]

    def read_inode(self, ino):
        group = (ino - 1) // self.inodes_per_group
        idx = (ino - 1) % self.inodes_per_group
        it = rd32(self.bgdt, group * 32 + 8)
        off = it * self.bs + idx * self.inode_size
        return self.img[off:off + self.inode_size]

    def get_block_num(self, idata, index):
        ptrs = self.bs // 4
        if index < 12:
            return rd32(idata, 40 + index * 4)
        index -= 12
        if index < ptrs:
            ind = rd32(idata, 40 + 12 * 4)
            if not ind: return 0
            return rd32(self.block(ind), index * 4)
        index -= ptrs
        if index < ptrs * ptrs:
            dind = rd32(idata, 40 + 13 * 4)
            if not dind: return 0
            ind = rd32(self.block(dind), (index // ptrs) * 4)
            if not ind: return 0
            return rd32(self.block(ind), (index % ptrs) * 4)
        return 0

    def list_dir(self, ino):
        """Return list of (name, inode, ftype) for a directory."""
        idata = self.read_inode(ino)
        size = rd32(idata, 4)
        nblocks = (size + self.bs - 1) // self.bs
        entries = []
        for bi in range(nblocks):
            b = self.get_block_num(idata, bi)
            if not b: break
            bdata = self.block(b)
            off = 0
            while off < self.bs:
                d_ino = rd32(bdata, off)
                rec_len = rd16(bdata, off + 4)
                name_len = bdata[off + 6]
                ftype = bdata[off + 7]
                if rec_len == 0: break
                if d_ino and name_len > 0:
                    name = bdata[off + 8:off + 8 + name_len].decode('ascii', errors='replace')
                    entries.append((name, d_ino, ftype))
                off += rec_len
        return entries

    def resolve(self, path):
        """Resolve path to inode number."""
        ino = 2
        if path == '/': return ino
        for comp in path.strip('/').split('/'):
            found = False
            for name, child_ino, ftype in self.list_dir(ino):
                if name == comp:
                    ino = child_ino
                    found = True
                    break
            if not found:
                raise FileNotFoundError(f"'{comp}' not found in path '{path}'")
        return ino

    def dump_superblock(self):
        print("=== Superblock ===")
        print(f"  Magic:        0x{self.magic:04X}")
        print(f"  Block size:   {self.bs}")
        print(f"  Total blocks: {self.total_blocks}")
        print(f"  Total inodes: {self.total_inodes}")
        print(f"  Inodes/group: {self.inodes_per_group}")
        print(f"  Inode size:   {self.inode_size}")
        print(f"  Groups:       {self.num_groups}")
        label = self.sb[120:136].rstrip(b'\x00').decode('ascii', errors='replace')
        print(f"  Label:        {label}")

    def dump_dir(self, path):
        ino = self.resolve(path)
        idata = self.read_inode(ino)
        size = rd32(idata, 4)
        nblocks = (size + self.bs - 1) // self.bs
        blocks = [self.get_block_num(idata, i) for i in range(nblocks)]
        entries = self.list_dir(ino)
        print(f"\n=== {path} (inode {ino}, size={size}, {nblocks} blocks: {blocks}) ===")
        for name, child_ino, ftype in entries:
            kind = 'd' if ftype == 2 else '-'
            child = self.read_inode(child_ino)
            child_size = rd32(child, 4)
            print(f"  {kind} ino={child_ino:4d}  {child_size:>8d}  {name}")
        print(f"  --- {len(entries)} entries ---")

    def dump_tree(self, path='/', depth=0):
        ino = self.resolve(path)
        entries = self.list_dir(ino)
        for name, child_ino, ftype in entries:
            if name in ('.', '..'): continue
            prefix = '  ' * depth
            kind = 'd' if ftype == 2 else '-'
            child = self.read_inode(child_ino)
            child_size = rd32(child, 4)
            full = path.rstrip('/') + '/' + name
            if ftype == 2:
                sub = self.list_dir(child_ino)
                real = [e for e in sub if e[0] not in ('.', '..')]
                print(f"  {prefix}{kind} {full}/ ({len(real)} entries)")
                self.dump_tree(full, depth + 1)
            else:
                print(f"  {prefix}{kind} {full} ({child_size} bytes)")

def main():
    img = 'disk/disk_ext2.img'
    dirs = []
    show_all = False
    args = sys.argv[1:]
    while args:
        if args[0] == '--dir':
            dirs.append(args[1])
            args = args[2:]
        elif args[0] == '--all':
            show_all = True
            args = args[1:]
        elif args[0].endswith('.img'):
            img = args[0]
            args = args[1:]
        else:
            args = args[1:]

    if not os.path.isfile(img):
        print(f"ERROR: {img} not found")
        sys.exit(1)

    ext2 = Ext2Inspector(img)
    ext2.dump_superblock()

    if show_all:
        print("\n=== Full tree ===")
        ext2.dump_tree('/')
    elif dirs:
        for d in dirs:
            ext2.dump_dir(d)
    else:
        ext2.dump_dir('/')
        ext2.dump_dir('/bin')
        ext2.dump_dir('/etc')

if __name__ == '__main__':
    main()
