## Vision
AIOS is a microkernel operating system built on seL4, designed for stability,
security, and AI-native development. External AI (Claude, etc.) generates and
reviews code, which is compiled and deployed to AIOS. The long-term goal is
self-hosted development within AIOS itself 

## Architecture
(see docs/ARCHITECTURE.md)

## AIOS Development Roadmap 
(see docs/ROADMAP.md)

## Milestone 1: POSIX Foundation (Current)
- [ ] libc with POSIX wrappers (open, read, write, close, stat, readdir)
- [ ] VFS server with file descriptor table
- [ ] Process server (sandbox lifecycle management)
- [ ] /dev/console for stdin/stdout
- [ ] Shell as a POSIX program
- [ ] Core utilities: ls, cat, echo, cp, rm, mkdir

## Future
- [ ] port to x86-64

## Architecture
(see docs/ARCHITECTURE.md)



## Legacy prototype

## Example Session (old version)
```
  ========================================
    AIOS Orchestrator v0.6
    Kernel:  seL4 14.0.0 (Microkit 2.1.0)
    Drivers: PL011 UART, virtio-blk, FAT16
    LLM:     llm_server (llama2.c engine)
  ========================================

  Boot: reading hello.txt...
    hello.txt: 50 bytes
    Contents: Hello from AIOS disk! This is our FAT filesystem.
    File closed.

  AIOS> load STORIES.BIN
  Loading model: STORIES.BIN
    File size: 60816028 bytes (59390 KiB)
    Loading: [##################################################] 100%
    Loaded 60816028 bytes into memory
    LLM server acknowledged model.
    Loading tokenizer...
    Tokenizer acknowledged. Ready for inference!

  AIOS> gen Once upon a time
  Generating...
  , there was a little boy named Timmy. Timmy loved to play at the
  beach and watch the waves. One day, he saw a big wave coming towards
  him. He ran as fast as he could and shouted, "Look, Mommy! Look at
  the big wave!"
  His mommy said, "Wow, that's a big wave! You're so lucky, Timmy."
  Timmy replied, "I am lucky! I'm the captain of the waves."
  Suddenly, a big wave came and knocked Timmy over.

  AIOS> shutdown
  Shutting down AIOS...
  System halted. Press Ctrl-A then X to exit QEMU.
```

## Quick Start

Prerequisites:

  - Microkit SDK 2.1.0 (https://trustworthy.systems/projects/microkit/)
  - AArch64 cross-compiler (aarch64-linux-gnu-gcc or aarch64-elf-gcc)
  - QEMU (qemu-system-aarch64)
  - mtools (mformat, mcopy) - brew install mtools or apt install mtools

Build and run:

  # 1. Download the model and tokenizer
  ```
  curl -L -o stories15M.bin https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.bin
  curl -L -o tokenizer.bin https://github.com/karpathy/llama2.c/raw/master/tokenizer.bin
  ```
  # 2. Create disk image and build
  ```
  make disk
  make clean && make run
  ```
  # 3. At the AIOS> prompt:
  ```
  load STORIES.BIN
  gen Once upon a time
  ```
Shell commands:

  help          - Show available commands
  cat <file>    - Read and display a file from disk
  load <file>   - Load model + tokenizer into memory
  gen <prompt>  - Generate text from prompt
  info          - Show system information
  shutdown      - Halt the system


## Project Structure
```
  aios/
  ├── README.md
  ├── LICENSE
  ├── Makefile
  ├── hello.system              Microkit system description
  ├── include/
  │   ├── aios/
  │   │   ├── channels.h        Channel ID definitions
  │   │   ├── ipc.h             Shared-memory layouts and constants
  │   │   ├── ring.h            Lock-free SPSC ring buffer
  │   │   └── serial.h          Serial output helpers
  │   ├── fat16.h               FAT16 on-disk structures
  │   └── virtio.h              virtio MMIO device structures
  ├── src/
  │   ├── serial_driver.c       PL011 UART driver PD
  │   ├── blk_driver.c          virtio-blk driver PD
  │   ├── fs_server.c           FAT16 filesystem server PD
  │   ├── orchestrator.c        Central coordinator + shell PD
  │   ├── llm_server.c          LLM inference engine PD
  │   └── echo_server.c         Stub PD
  └── scripts/
      └── mkdisk.sh             Disk image creation helper
```

## Key Design Decisions

  Formally verified kernel - seL4 provides mathematical proof of
  correctness, ensuring spatial and temporal isolation between
  protection domains.

  No libc - All PDs are freestanding (-nostdlib -ffreestanding). Math
  functions (expf, sqrtf, sinf, cosf), memory operations, and string
  utilities are implemented from scratch.

  Event-driven - No PD blocks. All I/O is asynchronous via Microkit
  notifications.

  FAT16 - Simple, well-understood filesystem. Read-only access with
  cluster caching for sequential reads.

  llama2.c - Andrej Karpathy's single-file Llama-2 implementation,
  adapted for bare-metal execution.

  QEMU 10.x compatible - Uses -kernel flag for loader image.


## Model Support

Any model compatible with karpathy/llama2.c checkpoint format:

  stories15M   - 15M params,  ~60 MB
  stories42M   - 42M params, ~168 MB
  stories110M - 110M params, ~440 MB

Download from: https://huggingface.co/karpathy/tinyllamas

For larger models, increase MODEL_DATA_MAX in include/aios/ipc.h and
the model_data region size in hello.system.


## Troubleshooting

QEMU shows no output:
  QEMU 10.x changed the -device loader behavior. Use -kernel instead
  (already set in the Makefile).

BLK: not a block device:
  The virtio-blk MMIO address may differ on your QEMU version. Dump
  the device tree with:
    qemu-system-aarch64 -machine virt,dumpdtb=virt.dtb
    dtc -I dtb -O dts virt.dtb | grep -A5 virtio

Model too large:
  Increase MODEL_DATA_MAX in include/aios/ipc.h and the model_data
  memory region size in hello.system. Ensure QEMU has enough RAM.


## License

MIT License. See LICENSE file.

The LLM inference engine is based on llama2.c by Andrej Karpathy
(MIT License). https://github.com/karpathy/llama2.c
