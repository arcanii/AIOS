# AIOS - AI Operating System on seL4

Project : Open ARIES

A minimal operating system built on the seL4 formally verified microkernel
that runs large language model inference in an isolated protection domain.

Built from scratch: seL4 kernel, Microkit, 6 protection domains, virtio-blk,
FAT16, 60MB model load, tokenizer, transformer inference, text generation.

## Plan
AIOS> build ls
  1. Orchestrator asks LLM: "generate ls.c for AIOS, 
     reference /ref/coreutils/ls.c"
  2. LLM reads the Linux reference, generates AIOS ls.c
  3. Orchestrator sends ls.c to TCC PD
  4. TCC compiles to aarch64 machine code
  5. Orchestrator loads code into sandbox PD
  6. Sandbox executes ls, output goes to serial

## Self-improvement demo (proof of concept)
- Day 1:  FAT16 write support (FS_CMD_WRITE, FS_CMD_CREATE).
- Day 2:  Put reference .c files on disk, add "ai build" command.  
- Day 3:  LLM generates code, saves to disk via orchestrator.
- Day 4:  Port TCC to a PD (compile in-memory).
- Day 5:  Sandbox PD with jump-to-code execution.
- Day 6:  Wire the full loop: prompt → generate → compile → run.
- Day 7:  Demo: "build ls" produces a working ls from scratch.
  
AIOS> build sh
  [same loop, but generates a shell interpreter]
  
AIOS> build net_driver
  [LLM reads Linux virtio-net source, generates 
   an AIOS virtio-net PD]



## What It Does

1. Boots seL4, initializes six isolated protection domains
2. Mounts a FAT16 filesystem from a virtio block device
3. Loads a 60 MB language model (stories15M) from disk into shared memory
4. Loads the tokenizer from disk
5. Generates text using a freestanding Llama-2 transformer inference engine
6. Provides an interactive serial shell (help, cat, load, gen, info, shutdown)


## Architecture

Six isolated protection domains running on seL4 via Microkit:

  serial_driver  - PL011 UART driver, RX/TX ring buffers (priority 254)
  blk_driver     - virtio-blk legacy v1, sector read/write (priority 240)
  fs_server      - Read-only FAT16, directory scan, FAT chain (priority 220)
  orchestrator   - Central coordinator, shell, file/model loading (priority 200)
  llm_server     - Llama-2 transformer inference, freestanding (priority 180)
  echo_server    - Stub for future use (priority 100)

Communication between PDs uses shared memory and Microkit notifications.

IPC channels:

  serial_driver <-ch1-> orchestrator <-ch4-> fs_server <-ch5-> blk_driver
                        orchestrator <-ch2-> blk_driver
                        orchestrator <-ch3-> echo_server
                        orchestrator <-ch6-> llm_server


## Example Session

  ========================================
    AIOS Orchestrator v0.4
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


## Quick Start

Prerequisites:

  - Microkit SDK 2.1.0 (https://trustworthy.systems/projects/microkit/)
  - AArch64 cross-compiler (aarch64-linux-gnu-gcc or aarch64-elf-gcc)
  - QEMU (qemu-system-aarch64)
  - mtools (mformat, mcopy) - brew install mtools or apt install mtools

Build and run:

  # 1. Download the model and tokenizer
  curl -L -o stories15M.bin \
    https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.bin
  curl -L -o tokenizer.bin \
    https://github.com/karpathy/llama2.c/raw/master/tokenizer.bin

  # 2. Create disk image and build
  make disk
  make clean && make run

  # 3. At the AIOS> prompt:
  #    load STORIES.BIN
  #    gen Once upon a time

Shell commands:

  help          - Show available commands
  cat <file>    - Read and display a file from disk
  load <file>   - Load model + tokenizer into memory
  gen <prompt>  - Generate text from prompt
  info          - Show system information
  shutdown      - Halt the system


## Project Structure

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
