# AIOS Model Training Tools

Train custom LLM models for AIOS inference in the llama2.c binary format.

## Requirements

python3 -m venv .venv source .venv/bin/activate pip install torch numpy sentencepiece datasets


## Quick Start

### 1. Prepare training data

Linux kernel C code (recommended for C generation)
python3 tools/prepare_data.py --source linux --tokens 30000000 --out out/data

Python code from codeparrot
python3 tools/prepare_data.py --source codeparrot --tokens 20000000 --out out/data

Your own C/H files
python3 tools/prepare_data.py --source dir:/path/to/code --tokens 10000000 --out out/data


### 2. Train

15M params (~58 MB .bin, ~30 min on M3 Max)
python3 tools/train.py --dim 288 --layers 6 --iters 5000

25M params (~96 MB .bin, ~1 hr on M3 Max)
python3 tools/train.py --dim 384 --layers 8 --iters 8000


### 3. Deploy to AIOS

cp out/model_final.bin code_model.bin make disk && make run


AIOS> load CODE_MOD.BIN AIOS> ai build HELLO


## Model Size Reference

| dim | layers | heads | params | .bin size | fits 128MiB? |
|-----|--------|-------|--------|-----------|-------------|
| 288 | 6      | 6     | 15M    | ~58 MB    | yes         |
| 384 | 8      | 8     | 25M    | ~96 MB    | yes         |
| 512 | 8      | 8     | 42M    | ~157 MB   | no          |

The AIOS model_data region is 128 MiB. After subtracting 16 MiB for
work memory and 1 MiB for tokenizer, the max model size is ~109 MB.
