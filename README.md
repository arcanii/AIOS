# AIOS
This is a thought experiment : what does a AI powered operating system look like? (A.I.O.S.). Ideally, this stuff is really in a white-paper, and this space is reserved more for getting it to work (To Be Actioned). At the momement, it is just a bunch of ideas all bouncing off each other.

The idea is that an AI OS would be able to discover, experiment and self-improve without risking stability of operation or data-corruption. It has to be a useable computer.

It behaves like a normal OS, akin to Linux, BSD/Unix, MacOS, and Windows, however it has more autonomy to change and improve itself automatically.
It would be cool if you could just run whatever programs you wanted (runs it all, insomuch as it is legaly permissible... MacOS apps may be a problem), and AIOS kept it all up to date, maximum efficiency, and free from risks. With high level direction, it could lock down, or protect data based on a high level policy.

Put AIOS on a computer and it could configure all the hardware, research what it didn't know and maybe even trial and error (aka reverse-engineer) hardware that it had no information about (a big stretch). Share centrally, or P2P with other AIOS so that they can improve as an ecosystem.

Is it better against viruses / malware? It could be, if the AI is programmed to detect, and mitigate viruses. The AI is watching the system operation and could detect if anything odd is happening. A good idea would to be make AIOS resistent if not well hardened against viruses and malware by design. No more ransomware.

It also has built in AI Orchestration capability, like OpenClaw, but at a fundamentally operational level of the OS.

This is where a micro-kernel is needed : small enough to be verified, strong enough that gives the AI the freedom to experiment (and fail). Personal computers should be fast enough to make this feasible. This is different than the design choice of Linux. It is probably not great for low power operations, so it could operate in an improving mode (with a LLM), and a static, low power mode (no self-improvement locally).

working name : project `Open ARIES` (acronym creator : have fun AI).
This project is inspired by Linux and all that makes Linux amazing : freedom and open source.

- ARIES = Autonomous Reasoning and Intelligent Execution System
- ARIES = Adaptive Reasoning and Integrated Environment System
- ARIES = Autonomous Runtime for Intelligent, Extensible Systems

Something like that.

What can we leverage (the lazy factor here); build our own microkernel? I'm nowhere near smart enough for that even though AI encourages me (don't limit yourself it says - ha ha).
Use seL4's microkernel as the core (tested, veried and open source)? - it can be kept pure and accomplish the same thing. Also have the option to branch it later if needed.
seL4 is a cool project that is operational and open-sourced.

- Linux binary compatibility is mandatory, so that ARIES is really a Linux derivative, and legally it should keep the same model (GPL2) as much as possible, but still allow BSD style licensing in clean room areas - the AI needs to log and audit anything it makes in clean-room conditions.
- Windows binary compatibility is extremely desired, but could be based on WINE functionality - get for free.
- MacOS binary compatubility is desired, but may carry legal baggage. There is `Darling` but sounds like an uphill battle.
- Mixed ISA - another project stretch. This would be like Rosetta 2 (Apple) or Prism (Microsoft) - something the AI can tackle later (ha ha ha).

What seL4 already provides (we delete our custom code that never really existed in the first place)

|Component|	Custom AIOS Kernel (dream)|	seL4 |	Notes|
|---------|---------------------------|------|-------|
|Memory manager|	mm.c (~2K LoC)|	Untyped Retype|	seL4's typed memory model; formally verified|
|IPC|	ipc.c (~1.5K LoC)|	seL4 IPC + Notifications	|~900 cycle round-trip on x86; faster than our design|
|Capability system|	capability.c (~2K LoC)|	CNodes + Badges	|Proven unforgeable, formally verified|
|Thread control|	thread.c (~1K LoC)	|TCBs + MCS scheduling	|Budget/period per-thread (the Orchestrator sets these)|
|IRQ routing|	irq.c (~500 LoC)	|IRQ Handler caps	|Forwarded as notifications to userspace PDs|
|Timer|	timer.c (~300 LoC)	|MCS kernel timer	|Sub-microsecond precision|
|Virtualization	|Not implemented	|vCPU objects	|Full Linux VM guest support, proven in camkes-vm|
|IOMMU|	Not implemented|	ARM SMMU / Intel VT-d	|DMA isolation for untrusted drivers|
|Multicore|	Not implemented|	SMP support	|Microkit 2.1.0 has SMP configurations|
|Formal proofs|	Aspirational|	Done (AArch32), in progress (MCS/x86/RISC-V)|	We inherit decades of verification work|

Net effect: We delete ~8K lines of unverified custom kernel code and replace it with 12K lines of formally verified seL4 code. 
We also gain virtualization, IOMMU, SMP, and a roadmap to full verification.

What we must build on top of seL4 :
|Component	|Why seL4 doesn't provide it|	Our approach|
|-----------|---------------------------|---------------|
|Dynamic PD management	|Microkit uses static XML-described systems	|Build a Component Manager PD with raw seL4 API access|
|AI Orchestrator|	Novel to AIOS	|Protection domain with max priority (254)|
|sDDF driver agents	|sDDF provides the framework; drivers are per-device	|AI generates/selects drivers from HKB|
|Linux ABI shim|	Not seL4's scope	|PD with syscall interposition|
|LLM inference engine|	Novel to AIOS	|Runs inside Orchestrator PD|
|Hardware Knowledge Base|	Novel to AIOS	|Shared memory regions + fs_server|
|Bootstrap protocol|	Novel to AIOS	|Custom UEFI loader talks to external AI|

### 1. DESIGN PHILOSOPHY

AIOS inverts the traditional OS model. Instead of a monolithic kernel with static drivers and schedulers, AIOS provides a minimal, formally-verifiable microkernel (~15K lines of C) that does only four things:

1. memory isolation.
2. IPC message passing.
3. capability-based security.
4. interrupt forwarding.

Everything else — device drivers, filesystem, scheduling, resource allocation — is managed by an AI Orchestrator running as a privileged userspace service, backed by a local LLM.
Hardware definitions are not compiled-in constants; they are living documents stored in a structured knowledge base that the AI can query, challenge, rewrite, and improve over time through observation and experimentation.

### 2. Core Principles

Minimality — The kernel does the absolute minimum: protect memory, pass messages, enforce capabilities, and route interrupts. 

Inspired by seL4's ~12K lines of formally verified C, AIOS targets a kernel small enough to be formally verifiable.

AI-First Resource Management — No static scheduler. No fixed driver model. The AI Orchestrator makes all policy decisions: which process runs, how memory is allocated, which hardware strategy is optimal. It does so by reasoning over a structured hardware knowledge base and live telemetry.

Evolvable Hardware Definitions — Device descriptors are stored as versioned, structured knowledge (not hardcoded tables). The AI can propose mutations, test them in a sandbox, measure results, and commit improvements. The system gets better at driving hardware over time.

Bootstrap from External, Run on Local — An external AI (cloud-based or network-adjacent) orchestrates the initial boot and hardware discovery. Once a local LLM is loaded and validated, the system cuts over to fully autonomous local operation.

### 3. MICROKERNEL CORE

The kernel is the only code running in Ring 0. It is designed to be formally verifiable (following seL4 precedent).
https://github.com/seL4/seL4

### 4. THE AI ORCHESTRATOR
This is the brain of AIOS, running as a privileged userspace process with full capability access. It contains the local LLM and all policy logic.

### 5. HARDWARE KNOWLEDGE BASE (HKB)
The HKB is the persistent, versioned repository of everything the system knows about hardware. It replaces /dev, device trees, ACPI tables, and driver databases.. HARDWARE KNOWLEDGE BASE (HKB). Using P2P, the AI is able to share / coordinate with other AIOS instances for improvement, but it is still locally verified and confirmed / signed by the orchestrator.

### 6. BOOTSTRAP SEQUENCE — EXTERNAL AI TO LOCAL AI
This is the most critical and novel part of the design. The machine begins with no local AI and must bootstrap one.

### 6.1 Boot Phases
|Phase 0|          Phase 1|           Phase 2|           Phase 3|          Phase 4|
|-------|-----------------|------------------|------------------|-----------------|
| UEFI/PXE    ──▶  |EXTERNAL AI  ──▶  |KERNEL UP    ──▶  |LOCAL AI     ──▶  |AUTONOMOUS|
| Firmware         |Bootstrap         |+ Services        |Loading           |Operation|
|                  |(Network)         |(Skeleton)        |+ Handoff         |         |

### 7. PRIORITY SERVICE SERVERS
Each runs as an isolated userspace process with capability-restricted access.

1. Filesystem Server

1. Display Server

1. Network Server

1. GPU/NPU Server

### 8. LOCAL LLM INFERENCE ENGINE
The Orchestrator's brain — embedded directly in the Orchestrator process, running on bare metal via ggml (the C tensor library behind llama.cpp).

#define DRIVER_SYSTEM_PROMPT \
    "You are the AIOS DriverAgent. You manage hardware device drivers.\n" \
    "You receive device telemetry and HKB entries. You can:\n" \
    "1. SELECT a driver_strategy from the HKB for a device\n" \
    "2. MODIFY a strategy's parameters\n" \
    "3. PROPOSE a new strategy as a mutation\n" \
    "4. DIAGNOSE device faults using register dumps\n" \
    "5. ROLLBACK a failed strategy to a previous version\n\n" \
    "Always respond in JSON with {\"action\": \"...\", \"params\": {...}}\n" \
    "Be conservative — prefer strategies with confidence > 0.7.\n" \
    "Only propose mutations when telemetry shows clear room for improvement."

### 9. HANDOFF PROTOCOL: EXTERNAL AI → LOCAL AI

### 10. SECURITY MODEL


┌──────────────────────────────────────────────────────────────┐
│                    SECURITY ARCHITECTURE                     │
│                                                              │
│  PRINCIPLE: Everything is capability-gated. The AI           │
│  Orchestrator has broad caps but cannot bypass the kernel.   │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ Kernel (TCB) — 15K LoC, formally verifiable             │ │
│  │  • Enforces memory isolation (MMU)                      │ │
│  │  • Validates all capability operations                  │ │
│  │  • Cannot be modified at runtime                        │ │
│  └─────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ AI Orchestrator — privileged but NOT omnipotent          │ │
│  │  • Holds master capability to ALL device caps            │ │
│  │  • Can delegate (sub)caps to service servers             │ │
│  │  • CANNOT execute arbitrary code in Ring 0              │ │
│  │  • All decisions auditable via telemetry log             │ │
│  │  • HKB mutations sandboxed before commit                │ │
│  └─────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ Service Servers — least-privilege                        │ │
│  │  • Each gets ONLY the caps it needs                     │ │
│  │  • fs_server: storage MMIO + DMA, nothing else          │ │
│  │  • display_server: framebuffer MMIO, nothing else       │ │
│  │  • Crash one → others unaffected (restart via AI)       │ │
│  └─────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ HKB Sandbox — mutation safety                            │ │
│  │  • New driver strategies tested in isolated address space│ │
│  │  • Watchdog timer: sandbox killed after timeout          │ │
│  │  • Only MMIO writes to target device allowed             │ │
│  │  • No network, no filesystem, no cross-device access    │ │
│  │  • Results measured, compared to baseline                │ │
│  └─────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ LLM Output Validation                                    │ │
│  │  • All LLM outputs parsed as structured JSON/YAML       │ │
│  │  • Schema validation before execution                   │ │
│  │  • Range checks on all numeric values                   │ │
│  │  • Known-bad pattern detection (e.g., writing to        │ │
│  │    kernel memory addresses, disabling MMU)               │ │
│  │  • Confidence thresholds per action type                │ │
│  └─────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘

### 12. Structure

### 13. KEY DESIGN TRADEOFFS & DISCUSSION
"Why not just use Linux with an AI daemon?" Linux's monolithic kernel has ~30M lines of code. A driver bug can crash the whole system. AIOS's microkernel has ~15K lines — small enough to formally verify. Every driver runs in its own isolated process. When the AI proposes a new driver strategy that crashes, only that sandbox dies; the rest of the system continues.

"Can an LLM really make scheduling decisions?" Not for every context switch — that would be far too slow. The two-tier design is critical: the LLM runs at ~10-100ms latency and is consulted for policy decisions (what heuristic rules should apply, how to handle anomalies, what to do about OOM). It then compiles those decisions into sub-microsecond fast-path functions that handle the normal case. The LLM is the policy author, not the hot-path executor.

"What if the local LLM hallucinates a bad driver init sequence?" Three safety layers protect against this. First, all LLM output is schema-validated — a malformed response is rejected before execution. Second, new strategies are tested in a sandboxed address space with a watchdog timer — if the sandbox hangs or crashes, the mutation is rolled back. Third, every strategy has a confidence score and a version history — the system can always revert to a previously-known-good strategy.

"What model size works?" For a machine with a dedicated GPU (8GB+ VRAM), a 7B parameter model at Q4 quantization (~4.5GB) provides strong structured reasoning capability. For CPU-only or memory-constrained systems, a 1-3B model at aggressive quantization can still handle most orchestration tasks, with more decisions deferred to heuristic fast-paths. The external AI selects the optimal model during bootstrap based on the actual hardware detected.

"What happens if the network is never available?" The bootstrap loader includes a fallback path (bootstrap_from_disk) that loads the kernel, modules, a seed HKB, and a default LLM model from local storage. The system can boot fully offline using pre-packaged artifacts, though the initial HKB will be generic rather than tailored to the specific hardware.



