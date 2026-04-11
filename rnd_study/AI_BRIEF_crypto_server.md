# AI_BRIEF_crypto_server.md

## Purpose

This document briefs a future session on integrating the AIOS crypto_server -- a ChaCha20-based CSPRNG service that provides /dev/urandom and getrandom() support via seL4 IPC.


## Background

AIOS currently lacks a cryptographic random number source. The crypto_server fills this gap as an isolated seL4 process that:

- Maintains a ChaCha20 CSPRNG (same algorithm Linux uses since 4.8)
- Collects entropy from ARM generic timer jitter and IPC timing deltas
- Auto-reseeds every 4096 blocks (256 KiB of output)
- Services random byte requests via standard seL4 IPC
- Provides forward secrecy: reseed XORs into the key, old state is unrecoverable


## Files Overview

Six Python scripts generate eight C/build files to /tmp/. All scripts use the standard AIOS workflow: cat to /tmp, idempotent with duplicate guards, no single-quotes in code comments.

### Scripts (run in order, or use the master runner)

    00_crypto_all.py            Master runner -- executes 01-05 in sequence
    01_crypto_chacha20.py       Generates crypto_chacha20.h and crypto_chacha20.c
    02_entropy_collect.py       Generates entropy_collect.h and entropy_collect.c
    03_crypto_server.py         Generates crypto_server.h and crypto_server.c
    04_posix_crypto_hooks.py    Generates posix_crypto_hooks.c (reference snippets)
    05_cmake_fragment.py        Generates crypto_server_cmake.txt

### Generated files

    /tmp/crypto_chacha20.h      CSPRNG type and API (init, reseed, generate)
    /tmp/crypto_chacha20.c      ChaCha20 quarter-round, block, and CSPRNG logic
    /tmp/entropy_collect.h      Entropy collector API
    /tmp/entropy_collect.c      Timer jitter sampling + IPC timing accumulator
    /tmp/crypto_server.h        IPC opcodes, constants, server entry point
    /tmp/crypto_server.c        Server main loop with RANDOM/RESEED/STATUS handlers
    /tmp/posix_crypto_hooks.c   Reference code (in comments) for POSIX wiring
    /tmp/crypto_server_cmake.txt  CMake fragment for building crypto_server


## How to Run

    python3 00_crypto_all.py

This writes all eight files to /tmp/. Re-running is safe -- existing files are skipped.


## Integration Steps

### Step 1: Place the source files

Create a directory for the crypto sources. Suggested location:

    src/crypto/

Copy the six C source and header files from /tmp/ into this directory:

    cp /tmp/crypto_chacha20.h src/crypto/
    cp /tmp/crypto_chacha20.c src/crypto/
    cp /tmp/entropy_collect.h src/crypto/
    cp /tmp/entropy_collect.c src/crypto/
    cp /tmp/crypto_server.h   src/crypto/
    cp /tmp/crypto_server.c   src/crypto/

### Step 2: Choose build strategy

Two options are provided in crypto_server_cmake.txt:

Option A -- Link into aios_root (simpler, runs as a thread in root server):

    set(CRYPTO_SOURCES
        src/crypto/crypto_chacha20.c
        src/crypto/entropy_collect.c
        src/crypto/crypto_server.c
    )
    target_sources(aios_root PRIVATE ${CRYPTO_SOURCES})
    target_include_directories(aios_root PRIVATE src/crypto)

Option B -- Standalone ELF (separate VSpace, better isolation):

    add_executable(crypto_server ${CRYPTO_SOURCES})
    target_include_directories(crypto_server PRIVATE src/crypto)
    target_link_libraries(crypto_server sel4 muslc)
    install(TARGETS crypto_server RUNTIME DESTINATION bin/aios)

Option A is recommended for initial integration since it avoids the need to allocate a separate VSpace, CNode, and TCB for the crypto server. Option B is the long-term target for proper isolation.

### Step 3: Create the server endpoint

In aios_root.c (or wherever servers are initialised), allocate an endpoint for crypto_server and start it:

    seL4_CPtr crypto_ep = /* allocate endpoint from untyped */;
    /* For Option A: spawn as a thread */
    create_thread(crypto_server_main, crypto_ep, /* priority, stack */);

Store crypto_ep somewhere accessible to the POSIX layer (e.g. a global in posix_internal.h or passed during POSIX init).

### Step 4: Wire up /dev/urandom reads

Reference code is in posix_crypto_hooks.c (all in comments). The key function is crypto_read_random() which sends CRYPTO_OP_RANDOM via IPC and unpacks the reply MRs into a user buffer. Add this to posix_file.c and dispatch to it when the fd points to /dev/urandom or /dev/random:

    if (fd_is_dev_urandom(fd) || fd_is_dev_random(fd)) {
        return crypto_read_random(user_buf, count);
    }

Both /dev/urandom and /dev/random use the same backing -- modern practice is to not distinguish them (Linux unified them in 5.6).

### Step 5: Wire up getrandom() syscall

SYS_getrandom is syscall 278 on AArch64. Add a handler in posix_misc.c that calls the same crypto_read_random() helper. Reference code is in posix_crypto_hooks.c snippet 3. Wire it in the syscall dispatch:

    case 278:  /* SYS_getrandom */
        result = sys_getrandom((void *)x0, (size_t)x1, (unsigned int)x2);
        break;

### Step 6: Register /dev/urandom and /dev/random nodes

In mkdisk.py or the ext2 builder, ensure /dev/urandom and /dev/random exist as recognised device paths. In the fd table, mark these with a device type flag so handle_read() can route to crypto_read_random().

### Step 7: Build and test

    rm -rf build-04
    cmake -G Ninja -B build-04 ...
    ninja -C build-04

Verify with a simple test from the mini shell:

    cat /dev/urandom | head -c 16 | hexdump

Or from a C test program:

    int fd = open("/dev/urandom", O_RDONLY);
    char buf[32];
    read(fd, buf, 32);


## IPC Protocol

Three opcodes, all via seL4_Call on crypto_ep:

### CRYPTO_OP_RANDOM (1)

Request: MR0 = 1, MR1 = nbytes (clamped to MR capacity, max ~480 bytes on AArch64)
Reply: MR0..MRn = random data packed as words

### CRYPTO_OP_RESEED (2)

Request: MR0 = 2, MR1..MR4 = up to 32 bytes of external entropy
Reply: empty (acknowledgement)

Use this to inject entropy from virtio-rng or other external sources.

### CRYPTO_OP_STATUS (3)

Request: MR0 = 3
Reply: MR0 = total reseed count, MR1 = blocks generated since last reseed


## Architecture Notes

### Entropy sources

Primary: ARM generic timer (CNTPCT_EL0) jitter. The entropy_collect_jitter() function samples the timer in a tight loop with busywork between reads. The LSBs of inter-sample deltas carry timing noise. Quality is lower on QEMU than bare metal but still usable for seeding.

Secondary: IPC message arrival timing. Each incoming IPC message contributes 1 bit (LSB of the inter-arrival delta). After 8 messages, one byte is fed into the CSPRNG via reseed.

Optional: virtio-rng. If QEMU is launched with -device virtio-rng-device, the host /dev/urandom is exposed as a hardware RNG. A future virtio driver could periodically feed high-quality entropy via CRYPTO_OP_RESEED.

### Auto-reseed

After every 4096 blocks (256 KiB) of output, the server automatically collects 32 bytes of fresh timer jitter and reseeds. This limits the damage if internal state is ever compromised.

### Forward secrecy

The reseed operation XORs new entropy into the ChaCha20 key bytes and resets the block counter. This means:
- Past outputs cannot be reconstructed even if current state leaks
- Known entropy does not reveal past key state

For stronger forward secrecy, a future enhancement could periodically generate 32 bytes and re-key the entire state (key erasure / fast key erasure pattern).

### Boot-time considerations

The initial seed is 48 bytes of timer jitter collected synchronously before the server enters its IPC loop. On QEMU without virtio-rng, this is the only seed quality gate. A future enhancement could have the server signal readiness via a notification object, with early getrandom() callers blocking until sufficient entropy is gathered.


## Known Limitations

- QEMU timer jitter has lower entropy density than bare metal hardware
- No virtio-rng driver yet -- must be added separately
- Maximum single-request size is limited by seL4 MR capacity (~480 bytes)
- No GRND_RANDOM / GRND_NONBLOCK flag handling in getrandom() yet
- No /dev/random blocking-until-entropy semantic (not needed -- follows Linux 5.6+ model)


## Dependencies

- No external libraries. Pure C with seL4 and string.h only.
- Requires aios_printf or equivalent for startup banner (stubbed with ifndef)
- CNTPCT_EL0 access requires EL1 or configured EL0 access (CNTKCTL_EL1)


## QEMU Tip

For best entropy quality during development, add this to your QEMU launch command:

    -device virtio-rng-device

This gives you host-backed hardware randomness. Once a virtio driver is implemented, it feeds directly into CRYPTO_OP_RESEED for high-quality entropy that supplements the timer jitter.
