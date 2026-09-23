# KnocOS: Goal & Vision

> **An AI-native operating system where intelligence is part of the kernel's world, not an app running on top of it.**

KnocOS aims to be a **production-grade** operating system with **local AI built in**: a large language model for heavy reasoning and conversation, and many **small neural networks (NNs)** trained for specific tasks that run cheaply on the CPU. Everything runs **locally**, with no cloud dependency. KnocOS machines can talk **directly to each other** over their own network, and users can bring their existing apps from **Windows, Linux and (partially) macOS**.

---

## 1. Core Idea: Two Levels of Intelligence

```text
                ┌──────────────────────────────────────┐
  Heavy work    │          Big Local LLM               │  chatting, planning,
  (on demand)   │  reasoning · coding · agent planning │  coding, complex tasks
                └──────────────────┬───────────────────┘
                                   │ called only when needed
                ┌──────────────────┴───────────────────┐
  Always-on     │        Small Task-Specific NNs       │  classify, detect,
  (low CPU)     │  file sorter · anomaly detector ·    │  predict, route
                │  intent classifier · context tracker │
                └──────────────────┬───────────────────┘
                                   │
                ┌──────────────────┴───────────────────┐
                │            KnocOS Kernel             │
                │  memory · scheduler · files · net    │
                └──────────────────────────────────────┘
```

| | Small NNs | Big LLM |
|---|---|---|
| **Size** | KBs – few MBs | GBs |
| **Runs** | Always, in the background | On demand |
| **Hardware** | CPU, very low load | CPU / GPU / accelerator |
| **Used for** | Classification, detection, routing, prediction | Chat, reasoning, planning, code, multi-step agent tasks |
| **Trained by** | Me, for one specific task each | Pre-trained open model, run locally |

**Rule:** if a small NN can do the job, the LLM stays asleep. This keeps the system fast and the CPU load low.

---

## 2. Feature Goals

### 🧠 OS-wide Context
KnocOS understands what you are working on across the whole system: open files, active projects, running apps and recent activity.
- *Small NN:* tracks activity and classifies the current "work context"
- *LLM:* uses that context when you ask for help

### 📁 Semantic File System
Find files by **meaning**, not just filename or path ("the invoice from last month", "my robot arm CAD file").
- *Small NN:* embedding model creates a vector for each file
- *Kernel/FS:* stores embeddings next to the file metadata as a vector index

### 🗂️ Automatic File Organization
Files go where they belong automatically, e.g. a WhatsApp video lands in `WhatsApp/Videos`.
- *Small NN:* file-type and source classifier
- *OS:* rule engine plus a user-reviewable move log (undo always possible)

### 🤖 Universal OS Agent
An AI agent that can operate system capabilities through **controlled tools**: coding, Blender, file management, settings, apps, all locally.
- *LLM:* plans multi-step tasks
- *OS:* exposes a typed **tool API** (open app, run command, edit file, drive Blender via its Python API, ...)
- Every action goes through the permission system below

### 🔐 Intent-based Security
The OS understands **what** an AI or app is trying to do, not just which syscall it made, and enforces permissions on that intent.
- *Small NN:* classifies action sequences (e.g. "reading many personal files, then network upload" → suspicious)
- *Kernel:* capability-based permissions, with user approval for sensitive intents
- *Built in v0.11.0:* per-program capabilities, `[SECURITY]` logging of denied system calls, and a system call trace the AI space reads when a program crashes (a program that made forbidden calls is not restarted). The trace is the future training data for the intent NN

### 🧠 Personal Knowledge Layer
Persistent memory about authorized files, projects and activity, so the AI knows *you*.
- Stored locally, encrypted, fully user-controlled (view, edit, delete)
- Only data the user has authorized is included

### 🩺 Self-Diagnosing OS
The OS investigates its own performance problems and errors and finds the likely cause.
- *Small NN:* anomaly detection on CPU, memory, disk and crash telemetry
- *LLM:* explains the problem in plain language and suggests fixes

### 🛠️ Self-Healing / Recovery
When possible, KnocOS automatically fixes or recovers from problems: restart failed services, roll back bad updates, restore corrupted config.
- Snapshots / checkpoints of system state
- Healing actions logged and reversible

### 🌐 KnocNet: OS-to-OS Communication Network
KnocOS machines communicate **directly with each other** (peer-to-peer), without a central server.
- Device discovery on local network and mesh
- End-to-end encrypted by default
- Share files, context and AI tasks between your own devices, e.g. a weak laptop hands a heavy LLM job to a stronger desktop
- Distributed inference: split a model across multiple KnocOS machines

### 🔄 Cross-Platform App Compatibility
People can move to KnocOS without losing their software.

| Platform | Format | Approach | Target |
|---|---|---|---|
| Linux | ELF | Linux syscall compatibility layer | Full |
| Windows | `.exe` (PE) | PE loader + Win32 API translation (Wine-style) | Broad |
| macOS | Mach-O | Mach-O loader + partial Darwin API layer (Darling-style) | Partial |

---

## 3. System Architecture (Target)

```text
┌─────────────────────────────────────────────────────────────────┐
│  Apps: native · Linux (ELF) · Windows (.exe) · macOS (partial)  │
├─────────────────────────────────────────────────────────────────┤
│  Compatibility Layers:  Linux ABI │ Win32/PE │ Darwin/Mach-O     │
├─────────────────────────────────────────────────────────────────┤
│  AI Services                                                     │
│   Universal Agent · Tool API · Knowledge Layer · Context Engine  │
│   LLM Runtime (big)  ·  NN Runtime (small, always-on)            │
├─────────────────────────────────────────────────────────────────┤
│  System Services                                                 │
│   Semantic FS · Auto-Organizer · Diagnostics · Self-Healing      │
│   Intent Security · KnocNet (P2P)                                │
├─────────────────────────────────────────────────────────────────┤
│  KnocOS Kernel                                                   │
│   processes · scheduler (AI-aware) · virtual memory · syscalls   │
│   filesystem · network stack · drivers (disk, net, GPU/NPU)      │
├─────────────────────────────────────────────────────────────────┤
│  Hardware: RISC-V (now) → x86-64 / ARM64 · GPU / NPU / FPGA      │
└─────────────────────────────────────────────────────────────────┘
```

---

## 4. Roadmap: From Kernel to AI-OS

Where KnocOS is **today**: boot, logging, physical pages, Sv39 paging, kernel heap, timer interrupts forwarded to the kernel, a Supervisor-mode trap handler, the PLIC interrupt controller, interrupt-driven keyboard input, a device driver model, a virtio-blk disk driver with permanent storage, processes with an **AI-aware scheduler**, an **AI space on its own CPU core that survives kernel crashes**, **fault containment** and a **warm kernel restart** so the AI never stops, **big memory** (RAM from the device tree, buddy allocator, 2 MiB megapages, spinlocks), **user mode + system calls** (capabilities, quotas, a system call trace the AI space reads), **wait queues** and the **KnocFS filesystem** (programs and model files on disk), automated tests and CI (see `README.md`).

| Stage | Focus | Key deliverables |
|---|---|---|
| **1. Kernel foundation** ✅ | Interrupts, traps, processes | ~~Trap handling~~ ✅, ~~timer heartbeat~~ ✅, ~~PLIC~~ ✅, ~~keyboard input~~ ✅, ~~device abstraction~~ ✅, ~~scheduler~~ ✅, ~~context switch~~ ✅, ~~AI-aware classes~~ ✅, ~~crash black box~~ ✅, ~~watchdog~~ ✅, ~~fault containment~~ ✅, ~~warm kernel restart~~ ✅, ~~user mode~~ ✅, ~~syscalls~~ ✅ |
| **1b. Resilient AI + memory for models** ✅ | Guardian, large RAM | ~~Crash black box~~ ✅, ~~watchdog~~ ✅, ~~isolated AI runtime space~~ ✅, ~~more RAM + large-memory support (2 MiB pages, memory map from the device tree)~~ ✅ |
| **2. Real OS** | Storage, drivers, userland | ~~virtio disk~~ ✅, virtio net, filesystem, ELF loader, shell, libc |
| **3. NN runtime** | Small AI inside the OS | Tensor math library (integer/quantized), NN model format, background inference service |
| **4. First small NNs** | Train & deploy task models | File classifier → auto-organization; embeddings → semantic search; anomaly detector → diagnostics |
| **5. LLM runtime** | Big local model | Quantized LLM inference (llama.cpp-style), memory-mapped model loading, AI-aware scheduling |
| **6. Agent & tools** | Universal OS Agent | Tool API, permission/intent security, knowledge layer, context engine |
| **7. KnocNet** | OS-to-OS network | TCP/IP stack, discovery, encrypted P2P protocol, shared tasks |
| **8. Compatibility** | Run other platforms' apps | Linux ABI → Windows PE/Win32 → partial macOS Mach-O |
| **9. Hardware** | Real machines & acceleration | x86-64/ARM64 ports, GPU/NPU drivers, custom RISC-V + FPGA AI accelerator |

### Stage 1 progress

| Step | Status | Why it matters for the AI-OS |
|---|---|---|
| Timer interrupts forwarded to the kernel | ✅ Done | The "heartbeat" the scheduler will use to share the CPU between apps, the LLM and background NNs |
| Supervisor trap handler | ✅ Done | Crashes are reported clearly instead of freezing, which is needed for self-diagnosis later |
| PLIC + interrupt-driven UART input | ✅ Done | The OS reacts to devices, the base for every driver (disk, network, GPU/NPU) |
| Power-off driver, automated tests (`make test`), CI | ✅ Done | Every change is checked automatically, the first step toward production quality |
| Device abstraction | ✅ Done | One common driver interface: the disk, network and GPU/NPU drivers the AI features need all plug in the same way |
| virtio-blk disk driver | ✅ Done | Permanent storage for files, and later for NN and LLM model files |
| Processes, context switch, AI-aware scheduler | ✅ Done | AI agent work gets the largest CPU share (60%), interactive work always responds first, background NNs never starve |
| AI space: black box + watchdog + recovery (v0.8.0) | ✅ Done | Core 1 survives kernel crashes and freezes, diagnoses them, saves a report and recovers |
| Fault containment + warm kernel restart (v0.9.0) | ✅ Done | A crashing process only kills itself, the AI decides the fix (restart it / disable its driver), and the AI never stops while the kernel restarts from a clean copy |
| Big memory (v0.10.0) | ✅ Done | Room for real AI models: RAM size from the device tree, a buddy allocator with 1 GiB blocks, 2 MiB megapages, spinlocks, a 256 MiB AI space |
| User mode + system calls (v0.11.0) | ✅ Done | Programs run in U-mode with their own page tables; checked system calls with capabilities and quotas; the AI sees each program's system calls and won't restart a suspicious one |
| Wait queues + filesystem (v0.12.0) | ✅ Done | Processes sleep until an event instead of polling; KnocFS stores programs, files and model files (contiguous extents); a host tool copies models onto the disk |
| Shell (v0.13.0) | 🚧 Next | Type commands: `ls`, `cat`, `ps`, `run`, `crashes`, `mem` |

README.md Phase 4 (interrupts) and Phase 5 (processes) together make up Stage 1 here.

---

## 4b. Milestone Roadmap: v0.8 → v1.0

Each milestone builds on the ones before it. Big milestones are split into sub-steps when we reach them.

### Phase A: AI that survives crashes

| # | Version | Milestone | What we build | Result |
|---|---|---|---|---|
| 1 | **v0.8.0** ✅ | **AI space (Guardian core)** | Core 1 runs a protected AI space (PMP-protected memory the kernel can't touch), heartbeat mailbox, crash + freeze detection, black box saved by the AI space, rule brain, reboot / safe mode / boot-loop protection | The kernel crashes and **core 1 keeps running**: it saves the report, diagnoses and recovers |
| 2 | **v0.9.0** ✅ | Fault containment + warm restart | A crashing process kills only itself, auto-restart of processes, disabling a bad driver on the next boot, the AI space restarting **only the kernel** from a clean copy | **The AI never stops**, even while the kernel restarts |

### Phase B: A real OS foundation

| # | Version | Milestone | What we build | Result |
|---|---|---|---|---|
| 3 | **v0.10.0** ✅ | Big memory | RAM size from the device tree, 2–4 GiB+, 2 MiB megapages, buddy page allocator, spinlocks for 2 cores, bigger AI region | Room for real AI models |
| 4 | **v0.11.0** ✅ | User mode + system calls | U-mode programs with their own page tables, `ecall` system calls, program loader | A buggy program can't hurt the OS |
| 5 | **v0.12.0** ✅ | Filesystem (KnocFS) | Files and folders on disk, `open/read/write/close`, a host tool to copy files (models) onto the disk | Files survive reboots, models live on disk |
| 6 | v0.13.0 | Shell + user programs | `knocsh` (`ls`, `cat`, `ps`, `kill`, `devices`, `crashes`, `mem`, `run`), a tiny C library | You type commands |

### Phase C: The first real AI inside KnocOS

| # | Version | Milestone | What we build | Result |
|---|---|---|---|---|
| 7 | v0.14.0 | Small NN runtime | Tensors, int8 quantized math, MLP → tiny transformer inference inside the AI space, Python training scripts that export weights | A neural network running on KnocOS |
| 8 | v0.15.0 | First trained NNs | Fault injection to produce labeled crash data, a crash classifier NN replacing the rule brain (rules stay as backup), a first intent classifier (Tier 1 router) | Real AI detects crashes and picks the fix |

### Phase D: The LLM agent

| # | Version | Milestone | What we build | Result |
|---|---|---|---|---|
| 9 | v0.16.0 | C library + porting layer | stdio, malloc, math, threads, mmap; port SQLite first | Existing C/C++ software runs on KnocOS |
| 10 | v0.17.0 | LLM runtime | llama.cpp port (GGUF), a small Qwen (0.5B–1.5B) loaded from the filesystem, an `ask` shell command | A local LLM answers questions |
| 11 | v0.18.0 | AI memory layer | `knoc-memory` (SQLite + sqlite-vec), embeddings, working/episodic/semantic/procedural memory, crash reports as memories | The AI remembers |
| 12 | v0.19.0 | Agent + tools + intent security | The Tier 0→3 router pipeline, permission-checked tool API, the agent in the AI_AGENT class | The agent does tasks safely |

### Phase E: Connected

| # | Version | Milestone | What we build | Result |
|---|---|---|---|---|
| 13 | v0.20.0 | Networking + KnocNet | virtio-net, TCP/IP (lwIP port), model download, a first KnocNet link between two KnocOS machines | Machines talk directly, models download |

### Phase F: Smooth GUI (last)

The GUI comes **after** the kernel, the AI, processes and the agent are complete. Its foundations (memory, user mode, filesystem, drivers) are built by then, so it isn't rewritten later.

| # | Milestone | What we build | Result |
|---|---|---|---|
| 14 | Framebuffer | virtio-gpu driver, pixels, fonts, the console on screen | A real screen |
| 15 | Input | virtio keyboard + mouse drivers | Pointer and clicks |
| 16 | Window system | A user-mode compositor, windows, basic widgets | Several apps on screen |
| 17 | KnocOS desktop | Taskbar, file manager, AI panel (AI space status, crash reports, LLM chat) | A smooth, complete OS experience |

### Road to v1.0
Real RISC-V hardware (with OpenSBI), x86-64 / ARM64 ports, RISC-V vector math, GPU/NPU drivers, Linux ABI → Windows `.exe` → partial macOS compatibility, and the full feature list above (semantic file system, auto-organization, OS-wide context, self-healing with rollback).

**Honest notes:** v0.11, v0.16 and v0.17 are the biggest steps. An LLM on QEMU will be slow (QEMU emulates the CPU), so it works as proof first; real speed comes with real hardware.

---

## 5. AI-Aware Scheduling ✅ (built in v0.7.0)

The scheduler treats AI work as a first-class citizen, without letting it freeze the machine.

| Class | Rule | Used for |
|---|---|---|
| **INTERACTIVE** | Always runs first, in tiny bursts | Keyboard, shell, UI: you never wait |
| **AI_AGENT** | Weight 60, 30 ms slices | Agentic tasks: coding agents, LLM planning (Tier 3) |
| **NORMAL** | Weight 30, 20 ms slices | Regular programs |
| **BACKGROUND** | Weight 10, 10 ms slices | Small always-on NNs (Tier 1), maintenance |

- **Largest share, not absolute priority:** an "AI always first" rule would freeze the keyboard and starve background work. Weighted sharing gives AI the most CPU while everything keeps moving
- **Measured:** the self-test runs an AI agent, a normal task and a background NN together and checks they get 60% / 30% / 10%
- **Later:** AI priority extends beyond the CPU, to memory (model memory never swapped out), the GPU/NPU queue and disk I/O for model loading

---

## 6. Resilient AI: the Guardian Architecture

**Goal:** when the OS breaks, the AI must still work, figure out what went wrong and help fix it. An AI inside the kernel would die in the same crash, so the AI is protected in layers.

| Layer | Where the AI runs | Survives | Real-world equivalent |
|---|---|---|---|
| **1. Protected AI service** | Its own user-mode process, outside the kernel | Crashed apps and buggy programs | Windows services |
| **2. Black box + recovery mode** | The kernel saves a crash report to disk. On reboot, a small recovery environment (small NNs) analyzes it | Kernel crashes | Windows Recovery Environment, Linux kdump |
| **3. Guardian** | A tiny monitor **below** the kernel (M-mode, later its own CPU core) that watches the kernel's heartbeat | A frozen kernel | Hardware watchdogs, management controllers |
| **4. KnocNet peer** | Another KnocOS machine diagnoses this one | A completely broken machine | Remote support |

- **Small NNs** are small enough to run in recovery mode and the guardian: they recognize crash patterns immediately
- **The big LLM** needs a healthy system: after recovery it reads the black box and explains the problem in plain language
- **Built in v0.8.0:** Layers 2 and 3 exist as the **AI space** on CPU core 1: PMP-protected memory the kernel can't touch, a heartbeat mailbox, crash and freeze detection, a rule brain (Tier 0), a black box on disk, and reboot / safe mode / boot-loop halt
- **Built in v0.9.0:** Layer 1 in its first form (**fault containment**: a crashing process stops alone, and the AI space decides whether to restart it or disable the driver it crashed in), and a **warm kernel restart**: the AI space keeps a clean copy of the kernel, stops core 0, restores it and restarts only the kernel, so the AI never goes down. The small NN (v0.15) and later an LLM replace the rule brain in the same place

---

## 7. AI Model Plan

### Router → worker design

```text
request / event → Tier 0: rules (no AI, never wrong)
               → Tier 1: small classifier NN (my own, closed set of tasks, confidence score)
               → Tier 2: small LLM router (only if Tier 1 isn't confident; constrained output)
               → Tier 3: big task LLM (does the work, AI_AGENT class, acts via the permission-checked tool API)
               → verification (compile, test, check results)
```

### Model roles

| Role | Size | Candidates (families, exact versions chosen later) |
|---|---|---|
| Embeddings (classifier input + semantic file system) | 20M–600M | Small Qwen / Gemma / BGE embedding models, MiniLM |
| Task classifier (Tier 1) | KBs–MBs | **My own trained NN** on top of embeddings |
| Small LLM (router fallback, recovery explanations) | 0.5B–4B | Small Qwen, Gemma, Llama, SmolLM instruct models |
| Coding agent | 7B–32B, quantized | Qwen Coder family, DeepSeek Coder |
| General chat / planning | 7B–14B, quantized | Qwen, Gemma, Llama, Mistral |

**Main family: Qwen.** It covers every size, is strong at coding and tool calling, and most models are Apache 2.0 (check each license before shipping).

### Rules
- **No model is hallucination-free.** The design makes mistakes harmless: closed-set classification, confidence thresholds, grammar-constrained output, verification of results, and grounding in real data
- **Model-agnostic:** models load from the standard **GGUF** format through a **model registry** (a config that maps roles to model files), so a better model is a file swap, not a code change
- **Runtime:** our own small int8 runtime for small NNs (Stage 3). A port of **llama.cpp** for big LLMs (Stage 5), which needs a filesystem, memory mapping, threads and a C library
- **Getting models onto KnocOS:** first by copying them onto the disk image from the host (built in v0.12.0: `make put FILE=model.gguf DEST=/models/model.gguf`), later by downloading them over KnocNet / TCP/IP

---

## 8. AI Memory Plan

The Personal Knowledge Layer, built on ideas from open-source AI memory projects (mem0, Letta/MemGPT, Zep/Graphiti, Cognee). Those projects are mostly Python and need a full Linux stack, so KnocOS **uses their designs and ports small C libraries**, and can run the originals later through Linux compatibility.

| Memory type | Holds | Example |
|---|---|---|
| Working | The current task | "Editing trap.c for the scheduler" |
| Episodic | What happened | "Kernel crashed yesterday: store page fault in vm.c" |
| Semantic | Facts | "The user's main project is KnocOS" |
| Procedural | How to do things | "Test with `make test`" |

- **Ideas used:** automatic fact extraction and updating (mem0), tiered core/archive memory (Letta), knowledge graph links (Zep/Graphiti, Cognee), decay of unused memories
- **Storage:** **SQLite** (a single portable C file) + **sqlite-vec** for search by meaning, inside a `knoc-memory` service running as its own protected process
- **Privacy:** local only, encrypted, only authorized data, and the user can view, edit and delete everything
- **Connected to the Guardian:** black box crash reports become memories, so the AI remembers past failures when diagnosing
- **When:** after the filesystem, user mode and a C library exist (goal.md Stage 6)

---

## 9. Key Challenges (Honest List)

- **LLM speed on CPU:** needs quantization (4-bit / 8-bit), SIMD/vector instructions, and eventually GPU/NPU drivers.
- **GPU drivers** are among the hardest parts of any OS, so early LLM work will be CPU-only.
- **Windows/macOS compatibility** is a very large surface. Wine and Darling took years, so start with Linux ELF and grow from there.
- **Privacy:** an OS that knows everything about the user must keep all data local, encrypted and user-controlled.
- **Safety:** an agent that can control the system needs strict, auditable permissions and undo.

---

## 10. Guiding Principles

1. **Local first:** no cloud required; your data never leaves your machine unless you say so.
2. **Small before big:** use a tiny NN whenever possible and wake the LLM only for heavy work.
3. **User in control:** every AI action is visible, permissioned and reversible.
4. **Understand every layer:** build from the hardware up; no black boxes.
5. **Easy migration:** people should be able to switch to KnocOS without losing their apps.

---

> **KnocOS: an operating system that understands you, runs its own intelligence, and connects directly to other minds like it.**
