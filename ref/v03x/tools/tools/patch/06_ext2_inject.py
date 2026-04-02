"""Patch tools/ext2_inject.py: add --disk-dir support."""

from .utils import set_module, log, read_file, write_file, replace_block

PATH = "tools/ext2_inject.py"

NEW_MAIN = '''def inject_tree(img_path, tree_dir):
    """Inject all files from a directory tree into the ext2 image.
    Files in etc/ go to /etc/, files in bin/ go to /bin/, others to /."""
    import os as _os
    files = []
    for root, dirs, fnames in _os.walk(tree_dir):
        for fn in fnames:
            full = _os.path.join(root, fn)
            files.append(full)
    if files:
        inject(img_path, files)


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: python3 tools/ext2_inject.py <image> <file1> [file2 ...]")
        print("       python3 tools/ext2_inject.py <image> --disk-dir <dir>")
        sys.exit(1)
    if sys.argv[2] == '--disk-dir':
        inject_tree(sys.argv[1], sys.argv[3])
    else:
        inject(sys.argv[1], sys.argv[2:])'''

def run():
    set_module("EXT2")
    log("=== Patching tools/ext2_inject.py ===")
    src = read_file(PATH)
    ok = True

    if '--disk-dir' not in src:
        old = ("if __name__ == '__main__':\n"
               "    if len(sys.argv) < 3:\n"
               '        print("Usage: python3 tools/ext2_inject.py <image> <file1> [file2 ...]")\n'
               "        sys.exit(1)\n"
               "    inject(sys.argv[1], sys.argv[2:])")
        src, s = replace_block(src, old, NEW_MAIN, "--disk-dir support")
        ok = ok and s
    else:
        log("--disk-dir already present, skipping")

    write_file(PATH, src)
    return ok
