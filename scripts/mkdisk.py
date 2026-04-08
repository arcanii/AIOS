#!/usr/bin/env python3
"""
AIOS ext2 Disk Image Creator

Usage: python3 scripts/mkdisk.py disk.img [size_mb] [--rootfs dir]
       [--install-elfs dir] [--aios-elfs dir] ...
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from ext2 import Ext2Builder

if __name__ == '__main__':
    output = 'disk/disk_ext2.img'
    size = 128
    rootfs = None
    elf_dirs = []
    aios_dirs = []
    sdk_dirs = []
    args = sys.argv[1:]
    while args:
        if args[0] == '--install-elfs':
            elf_dirs.append(args[1])
            args = args[2:]
        elif args[0] == '--aios-elfs':
            aios_dirs.append(args[1])
            args = args[2:]
        elif args[0] == '--install-sdk':
            sdk_dirs.append(args[1])
            args = args[2:]
        elif args[0] == '--rootfs':
            rootfs = args[1]
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
    b = Ext2Builder(size)
    b.build(output, rootfs=rootfs, elf_dirs=elf_dirs,
            aios_dirs=aios_dirs, sdk_dirs=sdk_dirs)
