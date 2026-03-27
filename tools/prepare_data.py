#!/usr/bin/env python3
"""
Prepare training data for AIOS code models.
Tokenizes source code into .bin files for train.py.

Usage:
    python3 tools/prepare_data.py --source linux --tokens 30000000
    python3 tools/prepare_data.py --source codeparrot --tokens 20000000
    python3 tools/prepare_data.py --source dir:/path/to/code --tokens 10000000
"""
import argparse
import glob
import os
import random
import sys

import numpy as np

def get_tokenizer():
    for path in ["tokenizer.model", "tools/tokenizer.model",
                  "/tmp/llama2c-train/tokenizer.model"]:
        if os.path.exists(path):
            tdir = os.path.dirname(os.path.abspath(path))
            if tdir not in sys.path:
                sys.path.insert(0, tdir)
            # Also add tools/ for tokenizer.py
            tools_dir = os.path.join(os.path.dirname(__file__))
            if tools_dir not in sys.path:
                sys.path.insert(0, tools_dir)
            from tokenizer import Tokenizer
            return Tokenizer(path)
    raise FileNotFoundError(
        "tokenizer.model not found. Place in project root or tools/ directory."
    )

def tokenize_files(file_list, target_tokens):
    tokenizer = get_tokenizer()
    all_tokens = []
    count = 0
    for fpath in file_list:
        try:
            sz = os.path.getsize(fpath)
            if sz < 100 or sz > 10000:
                continue
            with open(fpath, 'r', errors='ignore') as f:
                content = f.read()
            tokens = tokenizer.encode(content, bos=True, eos=True)
            all_tokens.extend(tokens)
            count += 1
            if count % 2000 == 0:
                print(f"  {count} files, {len(all_tokens):,} tokens")
            if len(all_tokens) >= target_tokens:
                break
        except Exception:
            continue
    print(f"Collected {len(all_tokens):,} tokens from {count} files")
    return all_tokens

def prepare_linux(out_dir, target_tokens):
    linux_dir = os.path.join(out_dir, "linux_src")
    tarball = os.path.join(out_dir, "linux-6.6.tar.xz")
    if not os.path.exists(linux_dir):
        print("Downloading Linux 6.6 kernel source (~133 MB)...")
        os.makedirs(linux_dir, exist_ok=True)
        os.system(f"curl -L https://cdn.kernel.org/pub/linux/kernel/v6.x/"
                  f"linux-6.6.tar.xz -o {tarball}")
        os.system(f"tar -xf {tarball} -C {linux_dir} --strip-components=1")
        if os.path.exists(tarball):
            os.remove(tarball)
    else:
        print(f"Linux source found at {linux_dir}")
    c_files = glob.glob(os.path.join(linux_dir, "**/*.c"), recursive=True)
    h_files = glob.glob(os.path.join(linux_dir, "**/*.h"), recursive=True)
    all_files = c_files + h_files
    random.shuffle(all_files)
    print(f"Found {len(c_files)} .c and {len(h_files)} .h files")
    return tokenize_files(all_files, target_tokens)

def prepare_codeparrot(out_dir, target_tokens):
    from datasets import load_dataset
    ds = load_dataset("codeparrot/codeparrot-clean", split="train", streaming=True)
    tokenizer = get_tokenizer()
    all_tokens = []
    count = 0
    for ex in ds:
        content = ex["content"]
        if len(content) < 50 or len(content) > 5000:
            continue
        tokens = tokenizer.encode(content, bos=True, eos=True)
        all_tokens.extend(tokens)
        count += 1
        if count % 5000 == 0:
            print(f"  {count} files, {len(all_tokens):,} tokens")
        if len(all_tokens) >= target_tokens:
            break
    print(f"Collected {len(all_tokens):,} tokens from {count} files")
    return all_tokens

def prepare_local(path, target_tokens):
    c_files = glob.glob(os.path.join(path, "**/*.c"), recursive=True)
    h_files = glob.glob(os.path.join(path, "**/*.h"), recursive=True)
    all_files = c_files + h_files
    random.shuffle(all_files)
    print(f"Found {len(all_files)} C/H files in {path}")
    return tokenize_files(all_files, target_tokens)

def save_tokens(all_tokens, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    arr = np.array(all_tokens, dtype=np.uint16)
    split = int(len(arr) * 0.95)
    train_path = os.path.join(out_dir, "train.bin")
    val_path = os.path.join(out_dir, "val.bin")
    arr[:split].tofile(train_path)
    arr[split:].tofile(val_path)
    print(f"Saved: {split:,} train + {len(arr)-split:,} val tokens")
    print(f"  {train_path}")
    print(f"  {val_path}")

def main():
    parser = argparse.ArgumentParser(description="Prepare training data for AIOS models")
    parser.add_argument("--source", default="linux",
                       help="linux, codeparrot, or dir:/path/to/code")
    parser.add_argument("--tokens", type=int, default=30_000_000,
                       help="Target token count (default: 30M)")
    parser.add_argument("--out", default="out/data",
                       help="Output directory for .bin files")
    args = parser.parse_args()

    if args.source == "linux":
        tokens = prepare_linux(args.out, args.tokens)
    elif args.source == "codeparrot":
        tokens = prepare_codeparrot(args.out, args.tokens)
    elif args.source.startswith("dir:"):
        tokens = prepare_local(args.source[4:], args.tokens)
    else:
        parser.error(f"Unknown source: {args.source}")
    save_tokens(tokens, args.out)

if __name__ == "__main__":
    main()
