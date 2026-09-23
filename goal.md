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

Where KnocOS is **today**: boot, logging, physical pages, Sv39 paging, kernel heap, timer interrupts forwarded to the kernel, a Supervisor-mode trap handler, the PLIC interrupt controller, interrupt-driven keyboard input, a device driver model, automated tests and CI (see `README.md`).

| Stage | Focus | Key deliverables |
|---|---|---|
| **1. Kernel foundation** 🚧 | Interrupts, traps, processes | ~~Trap handling~~ ✅, ~~timer heartbeat~~ ✅, ~~PLIC~~ ✅, ~~keyboard input~~ ✅, ~~device abstraction~~ ✅, scheduler, context switch, user mode, syscalls |
| **2. Real OS** | Storage, drivers, userland | virtio disk/net, filesystem, ELF loader, shell, libc |
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
| virtio-blk disk driver | 🚧 Next | Storage for files, and later for NN and LLM model files |
| Processes, context switch, scheduler | ⬜ | Run many programs at once, and later give AI workloads their own scheduling class |
| User mode + system calls | ⬜ | Isolate apps from the kernel, the base for intent-based security |

README.md Phase 4 (interrupts) and Phase 5 (processes) together make up Stage 1 here.

---

## 5. Key Challenges (Honest List)

- **LLM speed on CPU:** needs quantization (4-bit / 8-bit), SIMD/vector instructions, and eventually GPU/NPU drivers.
- **GPU drivers** are among the hardest parts of any OS, so early LLM work will be CPU-only.
- **Windows/macOS compatibility** is a very large surface. Wine and Darling took years, so start with Linux ELF and grow from there.
- **Privacy:** an OS that knows everything about the user must keep all data local, encrypted and user-controlled.
- **Safety:** an agent that can control the system needs strict, auditable permissions and undo.

---

## 6. Guiding Principles

1. **Local first:** no cloud required; your data never leaves your machine unless you say so.
2. **Small before big:** use a tiny NN whenever possible and wake the LLM only for heavy work.
3. **User in control:** every AI action is visible, permissioned and reversible.
4. **Understand every layer:** build from the hardware up; no black boxes.
5. **Easy migration:** people should be able to switch to KnocOS without losing their apps.

---

> **KnocOS: an operating system that understands you, runs its own intelligence, and connects directly to other minds like it.**
