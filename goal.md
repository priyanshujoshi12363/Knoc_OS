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
- *Built in v0.21.0:* the `organized` daemon sorts new downloads by itself, and a personal model trained inside KnocOS learns your own folders from your corrections
- *Built in v0.14.0:* `organize` in the knocsh shell, a 280K-parameter int8 classifier (type + source) with a rules layer and a `Random/` fallback, plan / apply / undo
- *Small NN:* file-type and source classifier
- *OS:* rule engine plus a user-reviewable move log (undo always possible)

### 🤖 Universal OS Agent
An AI agent that can operate system capabilities through **controlled tools**: coding, Blender, file management, settings, apps, all locally.
- *LLM:* plans multi-step tasks
- *OS:* exposes a typed **tool API** (open app, run command, edit file, drive Blender via its Python API, ...)
- Every action goes through the permission system below
- *Built in v0.18.0:* `agent` with rules first, then Qwen tool calling, 12 tools, any app with a manifest, y/n before every change, every action logged in the memory graph

### 🔐 Intent-based Security
The OS understands **what** an AI or app is trying to do, not just which syscall it made, and enforces permissions on that intent.
- *Small NN:* classifies action sequences (e.g. "reading many personal files, then network upload" → suspicious)
- *Kernel:* capability-based permissions, with user approval for sensitive intents
- *Built in v0.11.0:* per-program capabilities, `[SECURITY]` logging of denied system calls, and a system call trace the AI space reads when a program crashes (a program that made forbidden calls is not restarted). The trace is the future training data for the intent NN

### 🧠 Personal Knowledge Layer
Persistent memory about authorized files, projects and activity, so the AI knows *you*.
- Stored locally, encrypted, fully user-controlled (view, edit, delete)
- Only data the user has authorized is included
- *Built in v0.15.0:* KnocGraph, the memory graph (see section 8)

### 🩺 Self-Diagnosing OS
The OS investigates its own performance problems and errors and finds the likely cause.
- *Small NN:* anomaly detection on CPU, memory, disk and crash telemetry
- *LLM:* explains the problem in plain language and suggests fixes
- *Built in v0.16.0 / v0.17.0:* `healthd` (a multi-label NN on kernel telemetry) finds memory leaks, CPU hogs, disk thrashing, spawn storms and a filling disk and names the program; `ask what is wrong` explains it with facts from the memory graph

### 🛠️ Self-Healing / Recovery
When possible, KnocOS automatically fixes or recovers from problems: restart failed services, roll back bad updates, restore corrupted config.
- Snapshots / checkpoints of system state
- Healing actions logged and reversible
- *Built in v0.9.0 / v0.17.0:* the AI space restarts crashed programs, disables bad drivers and restarts the kernel; `healthd` lowers the priority of or stops the program causing a problem, and logs every fix

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

## 4. Where KnocOS Is Today (v0.24.0)

| Area | What works |
|---|---|
| **Kernel** | Boot on RISC-V (QEMU `virt`, 2 cores, 2 GiB), Sv39 virtual memory, buddy allocator, 2 MiB megapages, kernel heap, traps and interrupts, timer, PLIC, device drivers (UART, power, virtio disk), processes with an AI-aware scheduler and wake-up preemption, wait queues, fair sleep locks |
| **AI that survives crashes** | The AI space on core 1 (PMP-protected), a crash classifier NN inside it, black box, crash and freeze detection, fault containment, warm kernel restart, safe mode |
| **User space** | A C standard library (`libknoc`: stdio, stdlib, string, math, time...), U-mode programs, 29 system calls, capabilities and quotas, the KnocFS filesystem, the `knocsh` shell with scripts (`.ksh`) and `>` / `>>` for every command, installed apps from `/bin`, output capture |
| **Small AI** | File organizer (type + source classifier) that sorts Downloads by itself and learns your own folders, anomaly detector with self-healing and a live permission watch (`healthd`), memory graph (KnocGraph) with your work context |
| **LLM** | Qwen2.5-0.5B int8 with our own C engine; `chat` (a conversation that remembers) and `ask` (one question), GraphRAG from the memory graph, health and crash reports, the model chosen by `/etc/llm.model` |
| **Agent** | `agent`: rules first, then Qwen tool calling; 13 tools (scripts included), any app with a manifest in `/etc/apps`, y/n before changes, everything logged |
| **Quality** | `make test` (12 runs) in CI on every push |

**Honest limit:** under QEMU the LLM writes about one word per second, because QEMU emulates the CPU. Speed work waits for real hardware (see the Hardware track below).

---

## 4b. Version History (done)

| Version | Date | Milestone |
|---|---|---|
| v0.1.0 | 2026-09-10 | First boot: M-mode start, UART output, drop to S-mode |
| v0.2.0 | 2026-09-13 | Physical pages and Sv39 virtual memory |
| v0.3.0 | 2026-09-13 | Kernel heap |
| v0.4.0 | 2026-09-23 | Hardware interrupts and traps: timer, trap handler, PLIC, keyboard input, power-off driver, `make test`, CI |
| v0.5.0 | 2026-09-23 | Device abstraction (one driver interface) |
| v0.6.0 | 2026-09-23 | virtio disk driver: permanent storage |
| v0.7.0 | 2026-09-23 | Processes, context switch, AI-aware scheduler |
| v0.8.0 | 2026-09-23 | **AI space** on core 1: survives kernel crashes, black box, rule brain, safe mode |
| v0.9.0 | 2026-09-23 | Fault containment and warm kernel restart: the AI never stops |
| v0.10.0 | 2026-09-23 | Big memory: RAM from the device tree, buddy allocator, 2 MiB megapages, spinlocks |
| v0.11.0 | 2026-09-23 | User mode and system calls, capabilities, quotas, system call trace for the AI |
| v0.12.0 | 2026-09-23 | Wait queues and the KnocFS filesystem; programs and models live on disk |
| v0.13.0 | 2026-09-23 | The `knocsh` shell |
| v0.14.0 | 2026-09-24 | First AI model inside KnocOS: the file organizer |
| v0.15.0 | 2026-09-24 | Memory graph (KnocGraph): one shared memory for every AI model |
| v0.16.0 | 2026-09-24 | Anomaly detector (`healthd`) and the first LLM inside KnocOS (`ask`, Qwen2.5-0.5B) |
| v0.17.0 | 2026-09-24 | Self-healing (`healthd` fixes problems) and GraphRAG (`ask` answers from the memory graph) |
| v0.18.0 | 2026-09-25 | The agent: tools, app manifests, output capture, installed apps, shared LLM engine |
| v0.19.0 | 2026-09-26 | Shell scripts: variables, if / for / while, `>` and `>>` for every command, `copy` / `move`, startup script, the agent's `run_script` |
| v0.20.0 | 2026-09-26 | Chat: a conversation with the LLM that remembers, with memory-graph facts and the agent's tools |
| v0.21.0 | 2026-09-26 | Auto-organize and learning: a daemon sorts Downloads, a personal model learns your folders from corrections |
| v0.22.0 | 2026-09-26 | Crash classifier NN in the AI space: 10 diagnoses from real crashes, rules as the safety net |
| v0.23.0 | 2026-09-26 | Context tracker (`context`, facts for the LLM) and a live permission watch in `healthd` |
| v0.24.0 | 2026-09-26 | The C library `libknoc`: stdio, stdlib (malloc/free), string, ctype, math, time; `libctest` and `calc` |

---

## 4c. Roadmap (next)

Order: **features first, speed later** (no RISC-V hardware yet), and **the GUI last**. Each version is one milestone; big ones are split into sub-steps when we reach them.

### Phase 1: Automation and conversation

| Version | Milestone | What we build | Result |
|---|---|---|---|
| **v0.19.0** ✅ | Shell scripts | `.ksh` scripts in knocsh (variables, `$1`, `$?`, if / else / for / while, `exit`), `>` and `>>` for every command, `copy` / `move`, `/etc/startup.ksh`, the agent's `run_script` tool | You and the agent can automate tasks |
| **v0.20.0** ✅ | Chat | `chat`: a conversation with the LLM that remembers what you said; the agent's tools inside the chat | Talk to KnocOS like an assistant |
| **v0.21.0** ✅ | Auto-organize + learning | Downloads sorted by themselves; files you move back become training data | The organizer works alone and learns your files |

### Phase 2: Smarter small AI

| Version | Milestone | What we build | Result |
|---|---|---|---|
| **v0.22.0** ✅ | Crash classifier NN | A small NN in the AI space, trained on real crashes from crash scenarios run inside KnocOS | Real AI makes the diagnosis; the rules stay as the safety net |
| **v0.23.0** ✅ | Context + permission watch | `context` from the memory graph, context facts for the LLM; `healthd` watches denied calls while programs run | Context-aware help, and the OS spots programs that keep hitting permission walls |

### Phase 3: A userland for real software

| Version | Milestone | What we build | Result |
|---|---|---|---|
| **v0.24.0** ✅ | C library | `printf`, `malloc`, files, time, strings, math, standard headers | Normal C programs can be written for KnocOS |
| v0.25.0 | Compiler inside KnocOS | Port TinyCC; the agent can write, compile and fix C code | KnocOS builds its own programs; base of the coding agent |
| v0.26.0 | Kernel on several cores | SMP kernel, locks everywhere, programs on every core | Real multitasking, faster everything |

### Phase 4: Connected

| Version | Milestone | What we build | Result |
|---|---|---|---|
| v0.27.0 | Networking | virtio-net driver, TCP/IP, downloading files and models | KnocOS is online |
| v0.28.0 | KnocNet | Direct, encrypted links between KnocOS machines; shared files and AI jobs | OS-to-OS communication |
| v0.29.0 | Semantic search | Embeddings on files and graph nodes, search by meaning | "Find the invoice from last month" |

### Phase 5: Other systems' apps

| Version | Milestone | What we build | Result |
|---|---|---|---|
| v0.30.0 | Linux app compatibility | Linux ELF loader and system call layer | Linux programs run on KnocOS |
| later | Windows and macOS | `.exe` (Wine-style), partial macOS | Easy migration |

### Phase 6: Smooth GUI (last)

| Version | Milestone | What we build |
|---|---|---|
| v0.31.0 | Graphics | virtio-gpu framebuffer, pixels, fonts |
| v0.32.0 | Input | Mouse and keyboard events |
| v0.33.0 | Window system | Windows, compositing, apps drawing on screen |
| v0.34.0 | Desktop + AI panel | Desktop, chat with the LLM, memory graph viewer, organizer, health |

### Hardware track (when a board arrives, alongside the phases above)

| Step | What |
|---|---|
| Faster LLM | Parallel matrix math on the idle cores (QEMU multi-threaded mode already works), RISC-V vector instructions |
| Bigger brain | Swap in Qwen2.5 1.5B / 7B / Coder: only a new `.kllm` file and more RAM, no pipeline changes |
| Real machine | Boot on a RISC-V board (OpenSBI), later x86-64 / ARM64 and GPU/NPU drivers |

### v1.0

Everything above working together on real hardware: an OS that boots, survives its own crashes, organizes and heals itself, talks and acts through a local LLM, connects to other KnocOS machines, runs Linux apps, and has a smooth desktop.

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
- **Built in v0.9.0:** Layer 1 in its first form (**fault containment**: a crashing process stops alone, and the AI space decides whether to restart it or disable the driver it crashed in), and a **warm kernel restart**: the AI space keeps a clean copy of the kernel, stops core 0, restores it and restarts only the kernel, so the AI never goes down. Since v0.22.0 a crash classifier NN makes the diagnosis in the same place (the rules stay as the safety net), and later the LLM explains crashes in plain language

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
- **Model-agnostic:** built in v0.18.0. LLMs are converted to KnocOS's `.kllm` format (int8, tokenizer inside) by `models/llm/export.py`, and `/etc/llm.model` chooses which one runs, so a better model is a file swap, not a code change
- **Runtime:** our own small int8 runtime for small NNs (`user/nn.c`, v0.14.0) and our own LLM engine (`user/llm.c`, v0.16.0) instead of a llama.cpp port, which would need a full C library
- **Getting models onto KnocOS:** copied onto the disk image from the host (`make reset-disk DISK_MB=1024` puts Qwen on it, `make put FILE=... DEST=/models/...` adds others), later downloaded over TCP/IP (v0.27.0)

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
- **Built in v0.15.0 (first form):** KnocGraph, a knowledge graph in the kernel (4,096 nodes, 16,384 links on KnocFS). The organizer, the AI space (crash reports, verdicts, disabled drivers), the security checks and the kernel (started programs) all write to it; `memory` shows, explains (`memory why FILE`) and forgets. It moves to SQLite + vectors once the C library exists
- **Next:** SQLite + vectors after the C library (v0.24.0) and embeddings (v0.29.0)

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
