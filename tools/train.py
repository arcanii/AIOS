#!/usr/bin/env python3
"""
Train a tiny Llama 2 model for AIOS.
Produces a .bin file compatible with llama2.c / AIOS llm_server.

Usage:
    python3 tools/train.py --dim 288 --layers 6 --iters 5000
    python3 tools/train.py --dim 384 --layers 8 --iters 8000
"""
import argparse
import math
import os
import struct
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F

# Add llama2c model definition
LLAMA2C_DIR = os.environ.get("LLAMA2C_DIR", "/tmp/llama2c-train")
sys.path.insert(0, LLAMA2C_DIR)
from model import Transformer, ModelArgs

def get_device():
    if torch.cuda.is_available():
        return "cuda"
    elif hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        return "mps"
    return "cpu"

def get_batch(data, batch_size, seq_len, device):
    ix = torch.randint(len(data) - seq_len, (batch_size,))
    x = torch.stack([torch.from_numpy(data[i:i+seq_len].astype(np.int64)) for i in ix])
    y = torch.stack([torch.from_numpy(data[i+1:i+1+seq_len].astype(np.int64)) for i in ix])
    return x.to(device), y.to(device)

def get_lr(it, warmup, max_iters, max_lr, min_lr):
    if it < warmup:
        return max_lr * it / warmup
    if it > max_iters:
        return min_lr
    r = (it - warmup) / (max_iters - warmup)
    return min_lr + 0.5 * (1.0 + math.cos(math.pi * r)) * (max_lr - min_lr)

@torch.no_grad()
def estimate_loss(model, train_data, val_data, batch_size, seq_len, device, n_iters=20):
    model.eval()
    out = {}
    for name, data in [("train", train_data), ("val", val_data)]:
        losses = []
        for _ in range(n_iters):
            X, Y = get_batch(data, batch_size, seq_len, device)
            model(X, Y)
            losses.append(model.last_loss.item())
        out[name] = sum(losses) / len(losses)
    model.train()
    return out

def legacy_export(model, filepath):
    """Export to llama2.c legacy .bin format (v0)."""
    f = open(filepath, 'wb')
    p = model.params
    hd = model.layers[0].feed_forward.w1.weight.shape[0]
    shared = torch.equal(model.tok_embeddings.weight, model.output.weight)
    vs = -p.vocab_size if not shared else p.vocab_size
    nkv = p.n_heads if p.n_kv_heads is None else p.n_kv_heads
    f.write(struct.pack('iiiiiii', p.dim, hd, p.n_layers, p.n_heads,
                        nkv, vs, p.max_seq_len))
    def w(t):
        f.write(t.detach().cpu().view(-1).to(torch.float32).numpy().tobytes())
    w(model.tok_embeddings.weight)
    for l in model.layers: w(l.attention_norm.weight)
    for l in model.layers: w(l.attention.wq.weight)
    for l in model.layers: w(l.attention.wk.weight)
    for l in model.layers: w(l.attention.wv.weight)
    for l in model.layers: w(l.attention.wo.weight)
    for l in model.layers: w(l.ffn_norm.weight)
    for l in model.layers: w(l.feed_forward.w1.weight)
    for l in model.layers: w(l.feed_forward.w2.weight)
    for l in model.layers: w(l.feed_forward.w3.weight)
    w(model.norm.weight)
    w(model.freqs_cos[:p.max_seq_len])
    w(model.freqs_sin[:p.max_seq_len])
    if not shared:
        w(model.output.weight)
    f.close()
    mb = os.path.getsize(filepath) / (1024 * 1024)
    print(f"Exported {filepath} ({mb:.1f} MB)")

def main():
    parser = argparse.ArgumentParser(description="Train an AIOS code model")
    parser.add_argument("--dim", type=int, default=288, help="Model dimension")
    parser.add_argument("--layers", type=int, default=6, help="Number of layers")
    parser.add_argument("--heads", type=int, default=0, help="Attention heads (default: same as dim//48 or layers)")
    parser.add_argument("--seq-len", type=int, default=256, help="Max sequence length")
    parser.add_argument("--batch-size", type=int, default=32, help="Batch size")
    parser.add_argument("--iters", type=int, default=5000, help="Training iterations")
    parser.add_argument("--lr", type=float, default=3e-4, help="Peak learning rate")
    parser.add_argument("--data", default="out/data", help="Data directory with train.bin/val.bin")
    parser.add_argument("--out", default="out", help="Output directory")
    parser.add_argument("--name", default="model", help="Output model name (without .bin)")
    args = parser.parse_args()

    device = get_device()
    n_heads = args.heads if args.heads > 0 else max(args.dim // 48, 1)
    # Ensure n_heads divides dim
    while args.dim % n_heads != 0:
        n_heads -= 1

    model_args = dict(
        dim=args.dim, n_layers=args.layers, n_heads=n_heads, n_kv_heads=n_heads,
        vocab_size=32000, multiple_of=32, max_seq_len=args.seq_len, dropout=0.0,
    )

    # Load data
    train_file = os.path.join(args.data, "train.bin")
    val_file = os.path.join(args.data, "val.bin")
    if not os.path.exists(train_file):
        print(f"Error: {train_file} not found. Run tools/prepare_data.py first.")
        sys.exit(1)

    train_data = np.memmap(train_file, dtype=np.uint16, mode='r')
    val_data = np.memmap(val_file, dtype=np.uint16, mode='r')
    print(f"Data: {len(train_data):,} train + {len(val_data):,} val tokens")

    # Init model
    os.makedirs(args.out, exist_ok=True)
    conf = ModelArgs(**model_args)
    model = Transformer(conf).to(device)
    n_params = sum(p.numel() for p in model.parameters())
    est_mb = n_params * 4 / 1024 / 1024
    print(f"Model: dim={args.dim} layers={args.layers} heads={n_heads}")
    print(f"  {n_params/1e6:.1f}M parameters (~{est_mb:.0f} MB .bin file)")
    print(f"  Device: {device}")

    if est_mb > 109:
        print(f"WARNING: model ({est_mb:.0f} MB) may not fit in AIOS 128 MiB model_data region")

    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr,
                                   betas=(0.9, 0.95), weight_decay=0.1)

    warmup = min(100, args.iters // 10)
    best_val = float('inf')
    t0 = time.time()

    print(f"Training for {args.iters} iterations...")
    for it in range(args.iters):
        lr = get_lr(it, warmup, args.iters, args.lr, args.lr / 10)
        for pg in optimizer.param_groups:
            pg['lr'] = lr

        X, Y = get_batch(train_data, args.batch_size, args.seq_len, device)
        model(X, Y)
        loss = model.last_loss

        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        optimizer.step()
        optimizer.zero_grad(set_to_none=True)

        if it % 10 == 0:
            dt = time.time() - t0
            t0 = time.time()
            tok_s = args.batch_size * args.seq_len * 10 / max(dt, 0.001)
            print(f"  iter {it:5d}/{args.iters} | loss {loss.item():.4f} | "
                  f"lr {lr:.2e} | {tok_s:.0f} tok/s")

        if it > 0 and it % 500 == 0:
            losses = estimate_loss(model, train_data, val_data,
                                  args.batch_size, args.seq_len, device)
            print(f"  >>> eval: train={losses['train']:.4f} val={losses['val']:.4f}")
            if losses['val'] < best_val:
                best_val = losses['val']
                out_path = os.path.join(args.out, f"{args.name}.bin")
                legacy_export(model, out_path)
                print(f"  >>> new best val={best_val:.4f}")

    # Final export
    out_path = os.path.join(args.out, f"{args.name}_final.bin")
    legacy_export(model, out_path)
    print(f"\nDone! Best val loss: {best_val:.4f}")
    print(f"Model: {out_path}")
    print(f"\nTo use in AIOS:")
    print(f"  cp {out_path} code_model.bin")
    print(f"  make disk && make run")
    print(f"  AIOS> load CODE_MODEL.BIN")

if __name__ == "__main__":
    main()
