
# KnocOS

[![CI](https://github.com/priyanshujoshi12363/Knoc_OS/actions/workflows/ci.yml/badge.svg)](https://github.com/priyanshujoshi12363/Knoc_OS/actions/workflows/ci.yml)
![Version](https://img.shields.io/badge/version-v0.30.0-blue)
![Stage](https://img.shields.io/badge/stage-early%20development-orange)

**KnocOS** is being built as a **production-grade, AI-native operating system**, written from scratch. It is currently in **early development** (kernel foundation stage). See [`goal.md`](goal.md) for the long-term vision, [`notes.md`](notes.md) for a guided explanation of how everything works, and [`CHANGELOG.md`](CHANGELOG.md) for release history.

It is a bare-metal **RISC-V 64-bit** kernel that runs on the QEMU `virt` machine, written in C and RISC-V assembly with no standard library and no firmware (`-bios none`).

> **Learn → Build → Inspect → Debug → Understand → Repeat**

---

## Current Status

KnocOS currently has:

- RISC-V 64-bit bare-metal boot code (starts in **Machine mode**)
- PMP configuration so lower privilege modes can access all memory
- Drop from Machine mode → **Supervisor mode** via `mret`
- Custom linker script with separate `R-X` (text) and `RW-` (data) segments
- C kernel entry point with a 16 KiB kernel stack
- **UART driver** (16550): output plus **interrupt-driven keyboard input** into a ring buffer
- **PLIC interrupt controller**: device interrupts (UART, IRQ 10) delivered to the kernel
- **Device abstraction**: every driver has the same shape (`init`, `interrupt`, `read`, `write`) and is registered in a device table, like the Windows driver model and Device Manager
- **A shell, `knocsh`**: type commands (`ls`, `cd`, `cat`, `echo > file`, `ps`, `kill`, `mem`, `devices`, `crashes`, `ai`) and run programs from `/bin`; Ctrl-C stops the running program
- Kernel logging (`log_info`, `log_warn`, `log_trap`) and a `panic` handler
- **Device tree parsing**: RAM size and CPU count come from the firmware, not hard-coded (runs with 1 GiB to 8 GiB+, `make run` uses 2 GiB)
- **Buddy page allocator**: 4 KiB pages up to 1 GiB contiguous blocks, which merge back when freed (room for AI models)
- **Spinlocks**: the heap and page allocator are safe under preemption, and a shared UART lock keeps the kernel's and the AI space's lines from mixing
- **Sv39 virtual memory** (3-level page tables, all RAM identity-mapped with **2 MiB megapages**, paging enabled via `satp`)
- **Kernel heap** (`kmalloc` / `kfree`) in its own virtual region with block splitting, coalescing and automatic page growth
- **Timer interrupts at 100 Hz**: M-mode catches the hardware timer and forwards each tick to the kernel (S-mode), which counts it. Time is read with `rdtime`
- Machine-mode trap handler that saves/restores all registers and prints `mcause` for unhandled traps
- **Supervisor-mode trap handler in C**: exceptions are delegated to S-mode, decoded by name and reported with `scause` / `sepc` / `stval`
- Standalone `timer/` test program for reading `mtime`
- **Power-off / reboot** driver (QEMU test device): press **Ctrl-D** to shut KnocOS down
- **virtio-blk disk driver** (`disk0`): reads and writes 512-byte sectors of `disk.img` through interrupts, so data survives reboots
- **AI space (Guardian core)**: CPU core 4 runs a protected space the kernel can't touch (PMP hardware). It watches the kernel, and when the kernel crashes or freezes it **keeps running**: it diagnoses the problem, saves a crash report (black box) to disk, and **restarts only the kernel from a clean copy** (warm restart), so the AI itself never stops
- **Fault containment**: a crashing process is stopped alone, like a Linux "oops". The AI space diagnoses it and decides the fix: restart the process, leave it stopped if it keeps crashing, or **disable the driver** the crash happened in
- **KnocFS filesystem**: files and folders on the disk, stored as contiguous extents so large AI model files load fast. Programs load from `/bin`, files survive reboots, and `tools/knocfs.py` copies files (models) onto the disk from your PC
- **Wait queues**: processes sleep until an event wakes them (a key press, a finished disk read, a process exit) instead of polling. The disk has a sleep lock, so processes can use it safely at the same time
- **User mode + system calls**: programs run in RISC-V **U-mode** with their own page tables, so they can't touch the kernel, devices or each other. They ask the kernel for things through 9 **system calls** (`ecall`), checked against each program's **capabilities** and **memory quota**, and every call is recorded for the AI space. 16 system calls, including files (`open`, `read`, `write`, `readdir`...)
- **Processes and an AI-aware scheduler**: kernel processes with their own stacks, context switching, timer preemption, and 4 scheduling classes where **AI agent work gets the largest CPU share** (60%) while the keyboard stays instant and nothing starves
- **Automated tests** (`make test`) and **GitHub Actions CI** on every push
- Make-based build with automatic header dependencies and `-Wall -Wextra -Werror`

### Boot output

```text
[INFO] KnocOS v0.30.0 starting
[INFO] Supervisor interrupts enabled
[INFO] Device tree at 0x00000000BFE00000: RAM 2048 MiB at 0x0000000080000000, 8 CPUs
[INFO] Page memory initialized: 1791 MiB free, largest block 1024 MiB (buddy allocator)
[INFO] Virtual memory initialized
[INFO] Kernel page tables ready
[INFO] Kernel heap mapping prepared
[INFO] Enabling Sv39
[INFO] Sv39 enabled
[INFO] RAM mapped with 2 MiB megapages: 1026
[INFO] Kernel heap activated
[INFO] PMP verified: the kernel cannot read the AI space
[INFO] AI space online (core 4, 256 MiB protected at 0x0000000090000000)
[INFO] Spinlock verified: exclusive, interrupts off while held
[INFO] Allocation A successful
...
[INFO] Kernel heap 4.0 stress test passed
[INFO] Buddy allocator verified: 64 MiB contiguous block, 1024 single pages freed and merged back
[INFO] Large memory verified: 1024 MiB block at 0x00000000C0000000 read and written through megapages
[INFO] Supervisor timer interrupts verified
[TRAP] Breakpoint
[TRAP] sepc   = 0x0000000080001930
[INFO] Supervisor trap handler verified
[INFO] Device ready: uart0 (IRQ 10)
[INFO] Device ready: power0
[INFO] Device ready: disk0 (IRQ 1)
[INFO] Device ready: faulty0
[INFO] Devices: 4
       uart0  IRQ 10  ready
       power0  ready
       disk0  IRQ 1  2048 blocks  ready
       faulty0  ready
[INFO] Device table verified
[INFO] Disk sectors: 2048
[INFO] Disk write/read test passed
[INFO] Disk says: Hello from the host!     ← text written into disk.img from Linux
[INFO] Disk boot count: 1                  ← stored on the disk, +1 on every boot
[INFO] Black box: no previous crashes
[INFO] Scheduler started (AI-aware: INTERACTIVE first, then AI_AGENT 60 / NORMAL 30 / BACKGROUND 10)
[INFO] Workers created: agent-coder (AI_AGENT), normal-task (NORMAL), nn-sorter (BACKGROUND)
       PID  NAME            CLASS        STATE     CPU TICKS
       0    idle            IDLE         READY     1
       1    sched-test      INTERACTIVE  RUNNING   0
       2    guardian        BACKGROUND   SLEEPING  0
       3    agent-coder     AI_AGENT     READY     60
       4    normal-task     NORMAL       READY     30
       5    nn-sorter       BACKGROUND   READY     10
[INFO] CPU share: agent-coder 60% (expected 60%)
[INFO] CPU share: normal-task 30% (expected 30%)
[INFO] CPU share: nn-sorter 10% (expected 10%)
[INFO] AI-aware scheduling verified
[INFO] Preemption verified: CPU-bound workers never yield, all made progress
[INFO] Interactive wake latency (ticks): 0
[INFO] Interactive response verified
[hello] Hello from user mode! pid 6, uptime ticks 129   ← a program in U-mode, using system calls
[hello] got 4 KiB at 0x0000001100000000, slept and yielded, exiting
[badcall] kernel pointer, unmapped pointer and unknown call were all refused
[SECURITY] noperm (pid 8) called spawn without the SPAWN capability: denied
[noperm] spawn was refused: this program has no SPAWN capability
[hog] 8 MiB allowed, 16 MiB more refused: quota is 16 MiB
[bigmem] AI agent got 256 MiB at 0x0000001100000000 and used all of it
[INFO] User memory verified: every page and page table was returned when the programs exited
[INFO] User mode verified: 7 programs ran in U-mode with system calls, bad pointers refused, capabilities and quotas enforced
[INFO] All self-tests passed
[INFO] Console ready (Ctrl-D power off, Ctrl-C stop the running program)
[INFO] Test keys: Ctrl-F process fault, Ctrl-X driver fault, Ctrl-K kernel fault, Ctrl-O overwrite kernel code, Ctrl-P panic, Ctrl-W freeze
[INFO] Program keys: Ctrl-U run the crash program, Ctrl-E run the spy program

KnocOS shell (knocsh). Type help for commands.
knoc:/$ ls /bin                ← you type commands here
```

Example of an unhandled kernel fault (a store to an unmapped address):

```text
[TRAP] Store page fault
[TRAP] scause = 0x000000000000000F
[TRAP] sepc   = 0x0000000080000784
[TRAP] stval  = 0x0000000040000000
[TRAP] ra     = 0x000000008000077C
[TRAP] sp     = 0x00000000800080F0
[PANIC] Unhandled supervisor trap
```

---

## ✅ Just Completed: Search by Meaning (v0.30.0)

**Find files by what they are about.** Words don't have to match: "invoice" finds a file that says "electricity bill", "cooking" finds the pasta recipe, "program source" finds the C file:

```text
knoc:/$ find invoice from last month
find: from last month
   80%  /home/Documents/march_statement.txt  2026-08-22
        "Electricity bill for March. Account 7741, amount due: 4,500 rupees. Pl..."
knoc:/$ find resume
   75%  /home/work/profile_2026.txt  2026-09-26
        "Priya Sharma Software engineer Work experience: 4 years at a startup b..."
knoc:/$ find photos
find: image files
  100%  /home/Photos/goa_beach_trip.jpg  2026-09-26
```

- Dates (`last month`, `yesterday`, `this week`, `in march`, `2024`...) and types (`photos`, `pdf`, `videos`, `songs`, `code`) become filters
- KnocEmbed, a small model made for KnocOS (hashed words and letter pieces), learned meaning from the MiniLM sentence model on the host and runs in milliseconds inside KnocOS
- `indexd` keeps an index of `/home` in `/var/index`; `find` updates it first, so new files are found at once; `index status` shows it
- Files now have dates: `ls -l`
- `ask`, `chat` and the agent find memory-graph facts by meaning too, and "agent find ..." searches by meaning

## KnocNet (v0.29.0)

**KnocOS machines talk to each other, encrypted.** Pair two machines once with a code, then send files, fetch shared files, check each other's health, and ask the other machine's AI:

```text
beta$  knocnet pair wait
Pairing code: 424242 (valid 120 s). On the other KnocOS run:
  knocnet pair ADDRESS-OF-THIS-MACHINE 424242

alpha$ knocnet pair 10.0.2.2:41523 424242
paired with beta (e221-3f9e-6a35-96eb) at 10.0.2.2:41523
alpha$ knocnet status beta
beta: up 20 s, RAM 1787 of 2048 MiB free, disk 51 of 62 MiB free, AI space online, 0 kernel crashes, 0 warm restarts
alpha$ knocnet send beta /home/note.txt
sent note.txt (19 bytes) to beta: SAVED /home/KnocNet/alpha/note.txt 19
alpha$ knocnet get beta /home/Shared/shared.txt /home/got.txt
alpha$ knocnet ask beta what is 2+2
```

- Every machine has its own key (fingerprint shown by `knocnet id`); after pairing, machines trust each other by key, not by address
- Every connection makes fresh keys (X25519), both sides sign the handshake (ECDSA P-256), and every message is encrypted and checked (ChaCha20-Poly1305)
- `knocnetd` runs on an AI core and serves trusted machines: files arrive in `/home/KnocNet/NAME/`, only `/home/Shared/` can be fetched
- Two machines under QEMU: start the second with a port forward (`-netdev user,id=net0,hostfwd=tcp::7001-:7000`) and pair to `10.0.2.2:7001`

## 8 Cores, 4 for the Kernel and 4 for AI (v0.28.0)

**KnocOS runs programs on several cores at the same time, and the AI has its own cores.**

```text
knoc:/$ cpus
  CORE  ROLE      LOAD  RUNNING
  0     general   100%  knocsh
  1     general   100%  spin
  2     general   100%  spin
  3     general   100%  spin
  4     AI space  -     guardian: crash diagnosis, black box, warm restart
  5     AI        0%    -
  6     AI        0%    -
  7     AI        0%    -
```

- Cores 0-3 run the kernel, the shell and normal programs; core 4 is the protected AI space; cores 5-7 run the AI (`ask`, `chat`, `agent`, `healthd`, the organizer)
- Threads: `pthread_create` / `pthread_join` / mutexes in the C library, also for programs you build with `tcc`
- The LLM does its matrix math on the 3 AI cores: about 2x faster (same answer, 4 s instead of 8 s)
- `ps` shows the core of every process; a crash is still survived: the AI space stops all kernel cores, restores the kernel and starts them all again

## HTTPS and a Text Browser (v0.27.1)

**The whole web, from the KnocOS shell.** HTTPS works (BearSSL, with real certificate checks) and `web` is a text browser:

```text
knoc:/$ web en.wikipedia.org/wiki/RISC-V
RISC-V - Wikipedia
https://en.wikipedia.org/wiki/RISC-V

RISC-V (pronounced "risk-five") is a free and open standard instruction set
architecture[90] (ISA) based on reduced instruction set computer[91] (RISC)
...
-- lines 1-20 ... -- Enter more, NUMBER open link, h help, q quit
web> 91
```

- `web URL`: Enter for more, a number opens that link, `u` goes back, `/word` finds text, `d 5` downloads link 5, `h` for help
- `web -s risc-v`: searches the web (DuckDuckGo Lite; Google's results page needs JavaScript)
- `web -dump URL`: the page as plain text with its links; the agent reads web pages this way
- `fetch https://...` downloads over HTTPS; untrusted, expired or wrong-name certificates are refused
- New devices: a real-time clock (`date`) and a random number generator (HTTPS keys need real randomness)
- `/etc/hosts` gives names to machines: `host` is your PC (10.0.2.2)
- Limits: TLS 1.2 at most (a few TLS 1.3-only sites refuse), no JavaScript, no forms except search

## Networking (v0.27.0)

**KnocOS is online.** A virtio-net driver and a TCP/IP stack in the kernel (ARP, IPv4, ICMP, DNS, TCP), with three new commands:

```text
knoc:/$ net
net0: up
mac        52:54:00:12:34:56
address    10.0.2.15
netmask    255.255.255.0
gateway    10.0.2.2
dns        10.0.2.3
...
knoc:/$ ping 10.0.2.2
knoc:/$ fetch http://example.com/
knoc:/$ fetch http://10.0.2.2:8000/model.bin /models/model.bin
```

- `make run` gives KnocOS a network card through QEMU user networking: KnocOS is `10.0.2.15`, your PC is `10.0.2.2` (start `python3 -m http.server` on your PC to send it files)
- `fetch` downloads over HTTP and follows redirects; `https://` isn't supported yet (no TLS)
- Only installed programs in `/bin` get the new `NET` capability, and the agent can download through the `fetch` app
- Pinging internet hosts gets no answer under QEMU user networking; downloads from the internet work

## A C Compiler Inside KnocOS (v0.25.0)

**KnocOS builds its own programs.** TinyCC runs inside KnocOS, with the KnocOS C library as its standard library:

```text
knoc:/$ cd /home/code
knoc:/home/code$ tcc prog.c -o prog
knoc:/home/code$ ./prog 20
fibonacci(20) = 6765
knoc:/home/code$ tcc bad.c -o bad
/home/code/bad.c:6: error: ';' expected (got 'return')
```

- Several source files at once, errors with line numbers, programs run by path (`./prog`)
- Every program now has a working directory, so relative paths work everywhere
- The agent can use the compiler too (`run_app` with `tcc`) and reads the errors, the base of a coding agent

## The C Library (v0.24.0)

**KnocOS programs can be written in normal C.** `libknoc` gives them `printf`, `malloc` / `free`, `fopen`, `string.h`, `math.h`, `time.h` and the rest of the usual standard library:

```c
#include <stdio.h>
#include <math.h>

int main(int argc, char **argv)
{
    printf("sqrt(2) = %.5f, %d arguments\n", sqrt(2.0), argc - 1);
    return 0;
}
```

To add a program: put it in `user/NAME.c`, add `NAME` to `LIBC_PROGRAMS` in the `Makefile`, and run `make run`; it lands in `/bin` and runs like any other command. `libctest` checks the library (65 checks) and `calc` is a small calculator written this way:

```text
knoc:/$ calc 2 ^ 10
1024
knoc:/$ calc
calc> sqrt 2
= 1.41421
```

## Context and Live Permission Watching (v0.23.0)

**KnocOS knows what you're working on**, and notices programs that keep asking for things they may not do:

```text
knoc:/home/code$ context
Your recent work (from the memory graph, boot 1):
  Folders you work in: /home/code (5), /home/Downloads (3)
  Programs you use:    hello (1x), organize (1x)
knoc:/$ noperm repeat &
[SECURITY] noperm keeps asking for things it has no permission for (4 denied calls in 10 s)
[SECURITY] recovered: stopped noperm (pid 20)
```

- `ask`, `chat` and `agent` use your context when you ask about your work
- The permission watch works while a program runs, not only after it crashes, and follows the same recover / watch mode as the rest of `healthd`

## The Crash Classifier (v0.22.0)

**A neural network in the AI space diagnoses crashes.** It runs on core 4 inside the protected AI space, so it keeps working when the kernel crashes, and it tells apart crashes the old rules mixed up:

```text
knoc:/$ crash stack
[AI] Diagnosis: Stack overflow: the stack grew past its end (endless recursion or a huge local array).
[AI]   decided by: the crash classifier NN (stack_overflow, 100% sure)
knoc:/$ crash jump 5
[AI] Diagnosis: Jump to a bad address: a broken function pointer or return address (sepc = stval).
[AI]   decided by: the crash classifier NN (bad_jump, 100% sure)
```

- **10 diagnoses** for program and kernel crashes, trained on 1,590 real crashes produced inside KnocOS
- **Tiny and protected:** 4.7 KiB of int8 weights compiled into the PMP-protected AI space
- **Rules as the safety net:** when the NN is unsure (below 80%) or sees a kind of crash it never trained on, the rules decide; the actions (restart, disable a driver) stay rule-based

## Auto-Organize and Learning (v0.21.0)

**Downloads sort themselves, and KnocOS learns your folders.**

```text
knoc:/$ organize auto on
[ORGANIZE] 335505283.pdf -> Documents/335505283.pdf (ai)
[ORGANIZE] mystery.xyz -> Random/mystery.xyz (random)
knoc:/$ move /home/Downloads/Documents/335505283.pdf /home/College
knoc:/$ organize learn
[ORGANIZE] learned: 335505283.pdf belongs in /home/College
[ORGANIZE] personal model trained: 1 folder of yours
...a similar PDF is downloaded...
[ORGANIZE] 335505299.pdf -> /home/College/335505299.pdf (personal)
[ORGANIZE] todo.txt -> Text/todo.txt (ai)
```

- **Automatic:** the `organized` daemon sorts new files once they stop growing
- **Learns from you:** move a sorted file to your own folder and a small personal model (trained inside KnocOS, on top of the frozen base model) sends similar files there next time
- **Safe:** it only overrides the base model when it is sure and the file type fits; `organize --undo` still works; `organize forget` resets what it learned

## Chat (v0.20.0)

**Talk to KnocOS.** `chat` keeps Qwen loaded and remembers the whole conversation. It reads the memory graph when your message needs it and uses the agent's tools when you ask it to do something (always asking y/n before changing anything):

```text
knoc:/$ chat
KnocOS chat. Type a message, /help for commands, /exit to leave.
you: what files are in /home/Downloads?
knocos: list_folder(path=/home/Downloads)
...
you: /save /home/chat.txt
you: /exit
```

- **Fast follow-ups:** each new message only adds its own words to the model's memory; nothing is read twice
- **One brain for everything:** `chat`, `agent` and `ask` share the same engine, tools, safety rules and prompt cache
- `/new`, `/save FILE`, `/tools`, `/help`, `/exit`

## Shell Scripts (v0.19.0)

**knocsh can run scripts**, so you and the agent can automate tasks:

```text
# /home/backup.ksh
set folder $1
if exists $folder
    mkdir /home/backup
    for f in notes.txt todo.txt
        copy $folder/$f /home/backup
    end
    echo backup of $folder done
else
    echo no folder $folder
end
```
```text
knoc:/$ run /home/backup.ksh /home/Downloads
```

- Variables (`set`, `$NAME`, `inc`), arguments (`$1`, `$#`), the last exit code (`$?`)
- `if` / `else` / `end`, `for` / `end`, `while` / `end`, `exit N`; conditions: `exists PATH`, `A == B`, `not ...`, or any command
- `>` and `>>` for every command and program: `ps > /tmp/ps.txt`
- `copy` and `move`, and `/etc/startup.ksh` runs at every start
- The agent can run scripts too (`run_script`): it shows you the script and asks y/n first

## The Agent (v0.18.0)

**The LLM can do things, not only answer.** `agent` gets a task, uses tools and apps, asks you before changing anything, and logs what it did in the memory graph:

```text
knoc:/$ agent sort my downloads
[agent] plan (rules, no model needed): run_app
[agent] run_app(app=organize, args=/home/Downloads --apply) Allow? (y/n) y
...
14 files: 10 by the AI, 3 by the rules, 1 to Random/
knoc:/$ agent stop spin
[agent] stop_program(name=spin) Allow? (y/n) y
stopped spin
```

- **Small before big:** common requests are handled by rules in under a second; anything else goes to the language model, which uses Qwen's own tool-calling format in a loop (call a tool, read the result, call the next, answer)
- **Built to outlive the model:** the model is a setting (`/etc/llm.model`), tools are a table and apps describe themselves. A bigger Qwen (1.5B, 7B, Coder) only needs a new `.kllm` file
- **Any app:** put a program in `/bin` and a manifest in `/etc/apps` (name, description, usage, risk) and the agent can run it and read its output
- **Safe:** reading runs at once; writing, moving, stopping and running apps ask y/n; writes only in `/home` and `/tmp`; no delete and no raw shell

`agent --tools` lists everything it can use. `agent --call '{"name": "list_folder", "arguments": {"path": "/home"}}'` runs one tool without a model.

## Self-Healing and GraphRAG (v0.17.0)

**KnocOS fixes itself.** `healthd` finds the problem, names the program, and fixes it, even when two problems happen at once:

```text
knoc:/$ spin &
knoc:/$ spawner &
[HEALTH] CPU hog in spin (100% CPU, 100% sure)
[HEALTH] recovered: moved to background priority: spin (pid 19) because of the CPU hog
[HEALTH] spawn storm in spawner (2 programs started/s, 100% sure)
[HEALTH] recovered: stopped spawner (pid 20) because of the spawn storm
```

| Problem | Fix |
|---|---|
| CPU hog, disk thrashing | Move the program to background priority (it keeps running) |
| Memory leak | Stop the program after 10 s of steady growth |
| Spawn storm | Stop the program |
| Disk filling up | Stop the program when the disk is 90% full or would be full within 60 s |

`health watch` only reports, `health recover` (the default) reports and fixes. The shell and `healthd` are never touched, and the program you are waiting for is never called a CPU hog.

**The LLM knows this computer.** Before Qwen answers, `ask` looks up what the question needs in the memory graph, the health telemetry and the crash reports, and gives those facts to the model:

```text
knoc:/$ ask why was 335505283.pdf moved?
[ask] facts from the memory graph and the system:
  - organize moved 335505283.pdf from /home/Downloads to /home/Downloads/Documents because it is a document file (100% sure).
The computer moved the file 335505283.pdf from the Downloads folder to the Documents folder because it is a document file.
```

General questions (`ask What is an operating system?`) get no facts and stay fast. The fixed start of the prompt is cached on disk, so every question after the first skips about 40 prompt tokens.

## The Anomaly Detector and the First LLM (v0.16.0)

**KnocOS notices its own problems.** The kernel samples CPU, memory, disk and processes every second, and `healthd`, a small neural network running in the background, reads the last 10 seconds and says what is wrong and which program is doing it:

```text
knoc:/$ leak 2048 &
knoc:/$ [HEALTH] memory leak in leak (+2044 KiB/s, now 18 MiB, 100% sure)
knoc:/$ spin &
knoc:/$ [HEALTH] CPU hog in spin (100% CPU, 100% sure)
[HEALTH] memory leak in leak is over
```

It finds memory leaks, CPU hogs, disk thrashing, spawn storms and a disk filling up (99–100% on test runs it never saw), and every problem goes into the memory graph.

**And an LLM lives inside KnocOS.** `ask` runs Qwen2.5-0.5B-Instruct (int8, 528 MB) with our own C inference engine, no libraries:

```text
knoc:/$ ask What is an operating system? Answer in one sentence.
[ask] Qwen2.5-0.5B loaded in 11 s, reading 46 prompt tokens...
An operating system is a software program that manages and controls the hardware resources of a computer system, providing services such as file management, system calls, and device drivers.
```

It needs a disk of at least 1 GiB: `make reset-disk DISK_MB=1024`. Under QEMU it writes about one token every 1.4 seconds (QEMU emulates the CPU; real hardware is much faster).

## The Memory Graph, KnocGraph (v0.15.0)

**KnocOS remembers.** Every AI model and the AI space write what they learn and decide into one **knowledge graph** in the kernel. It survives reboots, and later the LLM will read it to understand the system (GraphRAG).

```text
knoc:/$ memory why /home/Downloads/Documents/335505283.pdf
/home/Downloads/Documents/335505283.pdf was moved here by organize from /home/Downloads/335505283.pdf, because:
  boot 1 +7s  organize: file /home/Downloads/335505283.pdf --classified_as--> type document
  boot 1 +7s  organize: file /home/Downloads/335505283.pdf --came_from--> source other
knoc:/$ memory show faulty0
driver faulty0:
  boot 1 +10s  ai-space: actor ai-space --disabled--> driver faulty0
  boot 1 +10s  ai-space: program console --crashed_in--> driver faulty0
knoc:/$ memory
Memory graph: 57 nodes, 57 links (room for 4096 / 16384), boot 2
```

- **Nodes:** files, folders, programs, drivers, crashes, file types, app sources, actors (the AI models and the kernel), diagnoses, actions, capabilities
- **Links:** `classified_as`, `came_from`, `moved_to`, `restored_to`, `started`, `crashed`, `crashed_in`, `diagnosed_as`, `action`, `disabled`, `denied`, `restarted`, `stopped`, `in_process`. Each one records **who** said it, the **confidence**, the **boot** and the **time**
- **Who writes:** the organizer, the AI space (crash reports, verdicts, disabled drivers), the security checks and the kernel. The actor always comes from the kernel, so a program can't write in another's name
- **You're in control:** `memory`, `memory recent`, `memory find`, `memory show`, `memory why`, `memory forget`
- Stored in `/memory` on the KnocFS disk (up to 4,096 nodes and 16,384 links; the oldest links are forgotten first)

**Next up:** the kernel on several cores.

Recent progress:

| Commit | Milestone |
|---|---|
| `9a094c3` | Integrate kernel timer interrupts |
| `42458f8` | Verify machine timer interrupts |
| `2a69e89` | Fix timer interrupt and reliable timer test, R-X / RW- linker segments |
| `6fe8a98` | Supervisor trap handler, timer interrupts forwarded to S-mode |
| `9bffe5d` | PLIC + interrupt-driven UART input, keyboard echo, one shared UART driver |
| `cb619ec` | Power-off driver, `make test`, CI, strict warnings, auto dependencies, version `v0.4.0` |
| `1ad97f8` | Device abstraction: device table, `uart0` and `power0` drivers, version `v0.5.0` |
| `42e7855` | virtio-blk disk driver (`disk0`), block devices in the device model, version `v0.6.0` |
| `bf051b7` | Processes, context switching, AI-aware scheduler, M-mode private stack, version `v0.7.0` |
| `04db193` | AI space on core 1 (PMP-protected), heartbeat mailbox, crash/freeze detection, black box, rule brain, safe mode, boot-loop protection, version `v0.8.0` |
| `2773100` | Fault containment, AI verdicts (restart process / disable driver), warm kernel restart from a clean copy, kernel code check, version `v0.9.0` |
| `4447fb6` | Device tree, buddy allocator, 2 MiB megapages, 256 MiB AI space, spinlocks + shared UART lock, 2 GiB RAM, version `v0.10.0` |
| `4197404` | User mode, per-program page tables, 9 system calls, capabilities, memory quotas, system call trace for the AI, ELF loader, 7 user programs, version `v0.11.0` |
| `5f6f06b` | Wait queues, sleep locks, multi-sector disk requests, KnocFS (extents), file system calls, programs from `/bin`, host tool, version `v0.12.0` |
| `2b9471a` | The shell `knocsh`, terminal layer, Ctrl-C, 6 system calls (`wait`, `ps`, `kill`, `sysinfo`, `devinfo`, `crashinfo`), version `v0.13.0` |
| `ed5d0bd` | File organizer AI (type + source classifier, rules layer, `Random/`), `/bin/organize`, FPU for programs, program arguments, `rename`, version `v0.14.0` |
| `9448bee` | Memory graph KnocGraph in the kernel, `graph` system call, `memory` shell commands, all AI models write to it, version `v0.15.0` |
| `a3adfcf` | Kernel telemetry, the anomaly detector `healthd`, fair sleep locks, faster disk, the Qwen LLM with our own engine (`ask`), version `v0.16.0` |
| `422d0ba` | Self-healing `healthd` (recover/watch, `setclass`), multi-label health model, GraphRAG for `ask`, prompt prefix cache, version `v0.17.0` |
| `f2e6d5f` | CI: tests wait for the shell prompt |
| `a6b2983` | The agent: tools, app manifests, output capture, installed apps, shared LLM engine, version `v0.18.0` |
| `dd6f3a9` | Shell scripts, redirection for every command, `copy`/`move`, startup script, agent `run_script`, version `v0.19.0` |
| `bcb9a63` | `chat` with conversation memory, shared assistant core (`assist.c`), version `v0.20.0` |
| `4dcc139` | Auto-organize daemon, learning from corrections (personal model), version `v0.21.0` |
| `63e18bb` | Crash classifier NN in the AI space, crash scenarios and data collection, version `v0.22.0` |
| `6bf2439` | Context tracker, live permission watch, version `v0.23.0` |
| `0ab4247` | The C library `libknoc`, `libctest`, `calc`, version `v0.24.0` |
| `a1f8493` | TinyCC inside KnocOS, working directories, programs by path, POSIX layer, version `v0.25.0` |
| `677c7e7` | Networking: virtio-net, TCP/IP stack, `net` / `ping` / `fetch`, `NET` capability, version `v0.27.0` |
| `3d801e2` | HTTPS (BearSSL), the `web` text browser, real-time clock, random numbers, `/etc/hosts`, version `v0.27.1` |
| `6814f5d` | 8 cores (4 kernel, AI space, 3 AI), big kernel lock, threads, LLM on the AI cores, `cpus`, version `v0.28.0` |
| `0cb52d1` | KnocNet: pairing, encrypted links, files and AI questions between machines, TCP servers, version `v0.29.0` |
| *(uncommitted)* | Search by meaning: `find`, KnocEmbed, `indexd`, file dates, memory facts by meaning, version `v0.30.0` |

What works right now:

- `boot.S` arms the first timer deadline, enables `mie.MTIE`, and sets `mcounteren.TM` so S-mode can use `rdtime`
- `machine_trap` handles machine timer interrupts: it calls `timer_interrupt()`, which does `mtimecmp += TIMER_INTERVAL` (no drift) and forwards the tick by setting `mip.SSIP`
- `mideleg` delegates the supervisor software interrupt to S-mode, and `trap_enable_interrupts()` turns on `sie.SSIE` and `sstatus.SIE`
- The kernel's `supervisor_trap_handler()` receives the forwarded tick, clears `sip.SSIP` and calls `timer_tick()`
- `boot.S` delegates exceptions to Supervisor mode with `medeleg = 0xB1FF`, covering misaligned/access faults, illegal instruction, breakpoint, U-mode `ecall` and page faults
- `supervisor_trap` (in `boot.S`) saves 31 registers into a `trap_frame_t`, calls `supervisor_trap_handler()` in C, restores the registers and `sret`s
- `supervisor_trap_handler()` reads `scause`, `sepc` and `stval`. A breakpoint (`ebreak`) is logged and skipped (`sepc += 4`), and anything else is decoded by name, printed, and ends in `panic`
- `mideleg` also delegates **external interrupts** (`SEIP`), and `trap_enable_interrupts()` turns on `sie.SEIE`
- On an external interrupt the handler **claims** the IRQ from the PLIC, calls `device_handle_irq()` to run the owning driver's interrupt handler (IRQ 10 → `uart0`, which moves received bytes into a 128-byte ring buffer), then **completes** the IRQ
- Drivers register in a **device table**, and `device_init_all()` starts each one and enables its IRQ in the PLIC
- `kernel_main` ends in an echo loop: `device_read(uart0)` reads from the buffer, and when it's empty the CPU sleeps with `wfi` until the next interrupt. Ctrl-D sends a power-off command to `power0` with `device_write()`
- `disk0` (virtio-blk) sends each sector request through a shared **virtqueue**, sleeps with `wfi`, and is woken by its interrupt (IRQ 1) through the same PLIC → `device_handle_irq()` path as the keyboard. `trap.c` and `plic.c` didn't change to support it
- **Processes:** each has its own 16 KiB stack, saved registers, a class and CPU accounting. `context_switch` (assembly) saves one process's registers and loads another's
- **Preemption:** every timer tick calls `scheduler_tick()`. When a process's time slice ends, the scheduler switches, even if the process never gives up the CPU
- **AI-aware scheduling:** INTERACTIVE processes run first. The rest share the CPU by weight (AI_AGENT 60, NORMAL 30, BACKGROUND 10) using virtual runtime, so nothing starves
- **Console as a process:** an INTERACTIVE kernel process reads the keyboard (sleeping until a key arrives), handles the control keys and passes the rest to the shell
- M-mode has its **own stack** (`mscratch`), so the timer handler never touches process stacks. The S-mode trap handler saves `sepc`/`sstatus` so a process switch inside a trap returns to the right place
- Self-tests: the disk test writes and reads back a sector, reads text placed in `disk.img` by the host, and increments a boot counter stored on the disk
- Self-tests: the timer test waits for 5 kernel ticks (1 s timeout), then checks the timer isn't running away (at most 1000 ticks in 30 ms; a busy host can deliver missed ticks in a burst, which is normal), and the trap test runs `ebreak` and checks the handler ran and returned

Next steps (full list in `goal.md`, section 4c):

- [x] v0.14.0 – v0.18.0: file organizer AI, memory graph, anomaly detector, LLM inside KnocOS, self-healing, GraphRAG, the agent
- [x] v0.19.0: shell scripts
- [x] v0.20.0: `chat`
- [x] v0.21.0: auto-organize + learning
- [x] v0.22.0: crash classifier NN in the AI space
- [x] v0.23.0: context tracker + live permission watch
- [x] v0.24.0: the C library
- [x] v0.25.0: a C compiler inside KnocOS
- [x] v0.27.0: networking
- [x] v0.27.1: HTTPS + text browser
- [x] v0.28.0: kernel on 8 cores (4 kernel + 4 AI)
- [x] v0.29.0: KnocNet
- [x] v0.30.0: search by meaning
- [ ] v0.31.0: Linux apps
- [ ] v0.32.0 – v0.35.0: the GUI (last)

---

## Architecture

```text
Architecture: RISC-V 64-bit
ISA:          RV64G
ABI:          LP64D
Machine:      QEMU virt
Kernel base:  0x80000000
RAM:          from the device tree (make run: 2 GiB, 0x80000000 – 0x100000000; minimum 1 GiB)
Cores:        8 (cores 0-3 = kernel and programs, core 4 = AI space, cores 5-7 = AI programs)
Page size:    4 KiB
Paging:       Sv39
```

### Privilege flow

```text
QEMU reset
   │
   ▼
_start (Machine mode)          boot/boot.S
   ├─ core 4? → aispace_boot → aispace_main(dtb) (M-mode, protected memory, never returns)
   ├─ cores 1-3, 5-7: wait until core 0 sets smp_go, then the same setup with their own stacks → secondary_main
   ├─ core 0: keep the device tree address (a1), wait for the AI space's clean copy
   ├─ core 0: clear .bss, set stack pointer
   ├─ PMP: block the AI space (0x90000000, 256 MiB), allow everything else
   ├─ satp = 0 (paging off), clear leftover interrupt state (a warm reboot keeps old CSRs)
   ├─ mtvec = machine_trap
   ├─ stvec = supervisor_trap
   ├─ medeleg: exceptions → S-mode
   ├─ mideleg: supervisor software + external interrupts → S-mode
   ├─ mstatus.MPP = S, MPIE = 1
   ├─ mcounteren.TM = 1 (rdtime)
   ├─ mtimecmp = mtime + TIMER_INTERVAL
   ├─ mie.MTIE + mie.MSIE = 1 (MSIE: the AI space can stop core 0)
   └─ mret
   │
   ▼
kernel_main(dtb) (Supervisor mode)  kernel/main.c
   ├─ trap_enable_interrupts()  sie.SSIE + sie.SEIE + sstatus.SIE
   ├─ fdt_parse()     RAM size and CPU count from the device tree
   ├─ page_init()     buddy allocator over all free RAM
   ├─ vm_init()       build Sv39 page tables (RAM with 2 MiB megapages)
   ├─ heap_init()     map first heap page
   ├─ vm_enable()     write satp, sfence.vma
   ├─ heap_activate() create first heap block
   ├─ guardian_init(), spinlock test
   ├─ heap stress test, buddy allocator test, 1 GiB block test
   ├─ timer interrupt test
   ├─ supervisor trap test (ebreak)
   ├─ plic_init()
   ├─ uart_register(), power_register(),
   │  virtio_blk_register()               fill the device table
   ├─ device_init_all()                   init each driver, enable its IRQ
   ├─ device_list()                       print the table
   ├─ disk self-test (write/read, host text, boot counter)
   ├─ process_init()                      kernel_main becomes the idle process (pid 0)
   ├─ create "sched-test" (INTERACTIVE)
   ├─ scheduler_start()
   └─ idle loop (wfi)
        sched-test: start 3 workers, sleep 1 s, check CPU shares, kill workers,
                    run the user programs (U-mode) and check their exit codes,
                    start "console" (INTERACTIVE) → it starts the shell knocsh

Timer interrupt ──► machine_trap (M-mode) ──► timer_interrupt(): re-arm + set mip.SSIP ──► mret
                         └──► supervisor_trap (S-mode) ──► clear sip.SSIP, timer_tick() ──► sret
Exception ────────► supervisor_trap (S-mode) ──► supervisor_trap_handler() ──► sret / panic
Key press ────────► UART ──► PLIC (IRQ 10) ──► supervisor_trap (S-mode)
                         ──► plic_claim() ──► device_handle_irq(10) ──► uart0 interrupt → ring buffer
                         ──► plic_complete() ──► sret
```

### Physical memory layout

```text
0x02004000 ─────────────── CLINT mtimecmp
0x0200BFF8 ─────────────── CLINT mtime
0x0C000000 ─────────────── PLIC
0x10000000 ─────────────── UART0 (IRQ 10)
0x10001000 ─────────────── virtio slot 0: disk0 (IRQ 1)
   ...
0x80000000 ─────────────── RAM START / kernel_start
     │  .text + .rodata          (R-X)
     │  .data + .bss             (RW-)
     │  .mailbox                 (shared with the AI space)
     ├──────────────────────── kernel_end
     │  Kernel stack (16 KiB)
     ├──────────────────────── stack_top
     │  Page info: 1 byte per page (512 KiB for 2 GiB)
     │  Free memory → buddy allocator (page tables, heap pages, big blocks)
0x90000000 ─────────────── AI SPACE (256 MiB)  ← PMP: the kernel cannot read, write or execute here
     │  AI space code (R-X) and data (RW-), stack (16 KiB)
     │  Clean copy of the kernel (for warm restarts)
     │  (rest reserved for the small NN runtime and models)
0xA0000000 ─────────────── free memory → buddy allocator
     │  ...
0xBFE00000 ─────────────── device tree (placed by QEMU, reserved)
0xC0000000 ─────────────── free memory: one 1 GiB block
0x100000000 ────────────── RAM END (with 2 GiB)
```

(Exact addresses of `kernel_end` / `stack_top` change as the kernel grows. Use `make size` or `nm` to see them.)

### Virtual memory layout (Sv39)

| Virtual range | Maps to | Flags | Purpose |
|---|---|---|---|
| `0x80000000 – RAM end` | same (identity), **2 MiB megapages** | `R W X` | Kernel + all RAM |
| `0x0C000000 – 0x0C400000` | same (identity) | `R W` | PLIC |
| `0x10000000` (1 page) | same (identity) | `R W` | UART |
| `0x10001000` (1 page) | same (identity) | `R W` | virtio-blk (slot 0) |
| `0x2000000000 – 0x2010000000` | pages from `page_alloc()` | `R W` | Kernel heap (grows on demand, far above RAM) |

---

## Project Structure

```text
KnocOS/
├── boot/
│   ├── boot.S        # M-mode entry, PMP, delegation, trap entry points (M and S), mret to S-mode
│   └── linker.ld     # Kernel layout, segments, stack
├── kernel/
│   ├── main.c        # kernel_main: init sequence + self-tests
│   ├── logging.c/h   # Log levels and panic(), built on the UART driver
│   ├── memory.c      # Early RAM / kernel / stack accounting helpers
│   ├── page.c/h      # Buddy page allocator (4 KiB pages up to 1 GiB blocks)
│   ├── fdt.c/h       # Device tree parser: RAM size, CPU count
│   ├── spinlock.c/h  # Spinlocks (interrupts off while held)
│   ├── cpu.c/h       # Several cores: per-core state, big kernel lock, wake-up signals, secondary boot
│   ├── rtc.c/h       # Real-time clock (goldfish RTC)
│   ├── virtio_rng.c/h # Random numbers (virtio-rng)
│   ├── virtio_net.c/h, net.c/h # Network card and the TCP/IP stack
│   ├── syscall.c/h   # System call dispatch, safe user copies, capabilities
│   ├── syscall_abi.h # System call numbers, errors, capabilities, user address layout
│   ├── elf.c/h       # ELF loader for user programs
│   ├── program.c/h   # Built-in program table (class + capabilities)
│   ├── programs.S    # Embeds the user program ELF files (.incbin)
│   ├── vm.c/h        # Sv39 page tables, mapping, satp enable, debug
│   ├── heap.c/h      # kmalloc / kfree kernel heap
│   ├── timer.c/h     # rdtime, tick counter, timer interrupt, shared timer constants
│   ├── trap.c/h      # Supervisor trap frame and C trap handler
│   ├── uart.c/h      # 16550 UART driver: output, RX interrupt, ring buffer
│   ├── plic.c/h      # PLIC: enable IRQs, claim / complete
│   ├── power.c/h     # Power off / reboot (QEMU test device), device power0
│   ├── device.c/h    # Device abstraction: driver struct, device table, IRQ dispatch
│   ├── virtio.h      # virtio-mmio registers and virtqueue structures
│   ├── virtio_blk.c/h# virtio-blk disk driver (disk0)
│   ├── process.c/h   # Processes and the AI-aware scheduler
│   ├── switch.S      # context_switch: save/restore registers between processes
│   ├── string.c/h    # memcpy / memset (needed by the compiler on bare metal)
│   ├── aispace.c/h   # AI space on core 4: watch, diagnose, black box, recover (self-contained)
│   ├── guardian.c/h  # Kernel side of the Guardian: mailbox, heartbeat, crash reporting, boot report, AI verdicts
│   ├── faulty.c/h    # faulty0: a test driver with a bug on purpose (Ctrl-X)
│   ├── mailbox.h     # Shared kernel ↔ AI space mailbox layout
│   └── blackbox.h    # On-disk crash report format
├── user/
│   ├── crt0.S        # Program entry: call main, then exit
│   ├── ulib.c/h      # System call wrappers, print, memset/memcpy
│   ├── linker.ld     # Programs are linked at 0x1000000000
│   └── *.c           # hello, badcall, noperm, hog, bigmem, crash, spy
├── timer/
│   ├── timer.S       # Standalone program that prints mtime in a loop
│   └── linker.ld
├── scripts/
│   └── test.sh       # Automated boot test used by `make test` and CI
├── .github/workflows/
│   └── ci.yml        # GitHub Actions: build + test on every push
├── Makefile
├── VERSION           # Current version (shown at boot)
├── CHANGELOG.md      # Release notes
├── README.md
├── goal.md           # Long-term AI-OS vision
└── notes.md          # Learning notes: how KnocOS works, basic to advanced
```

---

## Subsystems

### Boot: `boot/boot.S`

- Checks `mhartid`: **core 4 jumps to `aispace_boot`** (its own stack, then `aispace_main`); cores 1-3 and 5-7 wait for core 0 (`smp_go` in the mailbox), then get their own machine stack, timer and kernel stack and enter `secondary_main`; cores beyond 8 park
- Core 0 keeps the **device tree address** from `a1` (QEMU puts it there for every core) and passes it to `kernel_main(dtb)`. On a warm restart the AI space puts it back in `a1`
- Core 0 **clears `.bss`** (a warm reboot doesn't clear memory) and sets `sp` to `stack_top`
- **PMP:** entry 0 blocks the AI space region (`0x90000000`, 256 MiB, no permissions), entry 1 allows all other memory. The kernel (S-mode) cannot change PMP, so it can never reach the AI space
- Resets `satp` to 0 (paging off) and clears `sie`/`mie`/`mip`, because a warm reboot leaves old values in the CPU
- Installs `machine_trap` (`mtvec`) and `supervisor_trap` (`stvec`)
- Delegates exceptions to S-mode (`medeleg = 0xB1FF`) and the supervisor software + external interrupts (`mideleg = SIP_SSIP | SIP_SEIP`)
- Sets `mepc = supervisor_start`, `MPP = Supervisor`, `MPIE = 1`
- Enables `rdtime` for S-mode (`mcounteren.TM`), arms the first `mtimecmp`, enables `MTIE` and executes `mret`
- Includes `kernel/timer.h` for `CLINT_MTIME`, `CLINT_MTIMECMP` and `TIMER_INTERVAL`
- `mscratch` holds the top of a private 4 KiB **machine stack**. `machine_trap` swaps to it on entry (`csrrw sp, mscratch, sp`) and back on exit, so M-mode never uses (or depends on) the interrupted process's stack
- `machine_trap` saves 30 registers on a 256-byte frame, dispatches timer interrupts to C and prints the `mcause` hex value for anything else
- `supervisor_trap` saves 31 registers as a `trap_frame_t`, calls `supervisor_trap_handler(frame)` and returns with `sret`

### Linker script: `boot/linker.ld`

- Kernel starts at `0x80000000`
- Program headers: `text` (`FLAGS(5)` = R-X) for `.text`/`.rodata`, `data` (`FLAGS(6)` = RW-) for `.data`/`.bss`
- Exports `kernel_start`, `kernel_end`, `stack_bottom`, `stack_top`
- 16 KiB stack, 16-byte aligned

### Logging: `kernel/logging.c`

- `log_info()` → `[INFO] ...`
- `log_warn()` → `[WARN] ...`
- All output goes through the UART driver (`uart.c`)
- `log_trap()` → `[TRAP] ...`
- `log_trap_hex()` → `[TRAP] label0x0000000000000000`
- `panic()` → `[PANIC] ...` then halts forever

### Device tree: `kernel/fdt.c`

- QEMU (like real firmware) passes a **flattened device tree** in `a1`: a binary description of the machine
- `fdt_parse()` walks its tokens (`BEGIN_NODE`, `PROP`, `END_NODE`) and reads `/memory` → `reg` (RAM start and size, using the root `#address-cells` / `#size-cells`) and counts `/cpus/cpu@N`
- The device tree itself lives in RAM (QEMU puts it at `0xBFE00000`), so the page allocator reserves it. KnocOS needs at least **1 GiB**, otherwise QEMU places the tree inside the AI space

### Physical pages: `kernel/page.c` (buddy allocator)

- Free memory is kept as **blocks of 2^order pages**: order 0 = 4 KiB, order 9 = 2 MiB, order 18 = 1 GiB, each aligned to its own size, with one free list per order
- **Allocating** takes the smallest free block that fits and splits it in halves until it has the right size
- **Freeing** checks the block's **buddy** (`index ^ 2^order`, the half it was split from). If the buddy is free, they merge, and this repeats up to 1 GiB
- One metadata byte per page (free/allocated head + order), placed right after the boot stack: 512 KiB for 2 GiB of RAM
- Reserved at boot: kernel + stack + page info, the AI space, the device tree
- `page_alloc()` (one page), `page_alloc_order(order)`, `page_alloc_contiguous(bytes)`, `page_free(address)` (any block size, ignores bad or double frees)
- `page_total()`, `page_used()`, `page_free_count()`, `page_largest_free()`, `page_debug()` (free blocks per size)
- Protected by a spinlock, so it's safe when a timer tick switches processes in the middle of an allocation

### Spinlocks: `kernel/spinlock.c`

- `spin_lock()` turns interrupts off, then spins on an atomic swap (`amoswap`) until the lock is free. `spin_unlock()` releases it and restores interrupts
- Used by the heap and the page allocator. Turning interrupts off matters on one core too: otherwise a timer tick could switch to a process that then waits forever for the lock
- **The UART is shared by two cores:** a core owns it for a **whole line** (`console_owner` in the mailbox, taken with compare-and-swap, released at `\n`). A 10 ms timeout means a core that crashed while owning it can't block the other. `make test` checks that no line ever mixes kernel and AI space output

### Virtual memory: `kernel/vm.c`

- Sv39 3-level page tables (512 × 8-byte PTEs per table), tables allocated with `page_alloc()`
- `vm_map()` walks VPN[2] → VPN[1] → VPN[0] and creates intermediate tables on demand
- **Megapages:** a leaf entry in the level-1 table maps 2 MiB at once, with no level-0 table. `vm_map_range()` uses them whenever both addresses are 2 MiB aligned, and 4 KiB pages otherwise
- `vm_init(ram_start, ram_end)` identity-maps all RAM (`RWX`, megapages), the UART, power, virtio and PLIC (`RW`)
- `vm_enable()` writes `satp` (mode 8 = Sv39) and flushes the TLB with `sfence.vma`
- `vm_debug(va)` prints the full page-table walk for an address

### Kernel heap: `kernel/heap.c`

- Virtual heap region `0x2000000000 – 0x2010000000` (moved from `0x90000000`, which is now real RAM)
- `kmalloc` / `kfree` are protected by a spinlock
- Linked list of blocks with header `{ size, free, next }`, 8-byte aligned allocations
- **First-fit** allocation
- **Splitting**: large free blocks are split to fit the request
- **Coalescing**: adjacent free blocks merge on `kfree`
- **Automatic growth**: when no block fits, a new physical page is allocated and mapped at `heap_end`, and the last free block is extended
- `kfree` ignores `NULL`, pointers not in the heap and double frees
- Two-phase init: `heap_init()` maps the first page *before* paging, and `heap_activate()` writes the first block header *after* Sv39 is on (`0x2000000000` is only reachable through the page tables)

### Timer: `kernel/timer.c`

- `timer.h` holds constants shared with `boot.S`: `CLINT_MTIME`, `CLINT_MTIMECMP`, `TIMER_FREQ_HZ` (10 MHz), `TIMER_TICK_HZ` (100) and `TIMER_INTERVAL`
- `timer_read()` uses the `rdtime` instruction, which works in S-mode and after Sv39 is on
- `timer_interrupt()` (called from M-mode) does `mtimecmp += TIMER_INTERVAL` and forwards the tick to S-mode by setting `mip.SSIP`
- `timer_tick()` (called from the S-mode trap handler) increments `ticks`
- `timer_ticks()` returns the tick count
- The timer hardware is only written from M-mode

### Traps: `kernel/trap.c`

- `trap_frame_t` matches the register layout saved by `supervisor_trap`
- `supervisor_trap_handler()` reads `scause`, `sepc` and `stval` and decodes the cause into a name (page faults, illegal instruction, access faults, ...)
- Forwarded timer ticks (supervisor software interrupt) clear `sip.SSIP` and call `timer_tick()`
- External interrupts: `plic_claim()`, `device_handle_irq(irq)` (warning if no device owns it), `plic_complete()`
- Breakpoints are logged, counted and skipped (`sepc += 4`)
- **Fault containment:** an exception in a process that ran with interrupts on (`sstatus.SPIE`) prints an `[OOPS]` line and stops only that process (`process_crash()`). The guardian process reports it to the AI space
- Every other trap (idle/boot code, interrupt handlers, code with interrupts off) prints `scause`, `sepc`, `stval`, `ra` and `sp`, then panics
- `trap_breakpoint_count()` is used by the self-test
- `trap_enable_interrupts()` enables supervisor interrupts (`sie.SSIE`, `sie.SEIE`, `sstatus.SIE`)
- `trap.h` holds `SIP_SSIP`, `SIP_SEIP`, `SIE_SSIE`, `SIE_SEIE` and `SSTATUS_SIE`, shared with `boot.S`

### UART driver: `kernel/uart.c`

- NS16550 UART at `0x10000000`, IRQ 10, registered as device **`uart0`**
- `init`: 8N1, FIFOs on, "receive data available" interrupt on
- `interrupt`: drains every received byte into a 128-byte ring buffer (bytes are dropped if it's full)
- `read`: returns the bytes waiting in the buffer (0 if empty), and `write`: sends bytes
- `uart_putc()`, `uart_puts()`, `uart_put_hex()`, `uart_put_uint()` stay public as the **early console**: `logging.c`, `panic()`, `page.c` and `vm.c` use them directly, so crash messages work even before (or without) the device system

### Interrupt controller: `kernel/plic.c`

- PLIC at `0x0C000000`, using hart 0's **Supervisor context** (context 1)
- `plic_init()` sets the priority threshold to 0 (accept every enabled IRQ)
- `plic_enable(irq)` gives the IRQ priority 1 and turns on its enable bit
- `plic_claim()` returns the pending IRQ number (0 = none), and `plic_complete(irq)` tells the PLIC it was handled

### Power: `kernel/power.c`

- Uses QEMU's test device at `0x00100000` (mapped in `vm.c`)
- `power_off()` writes `0x5555` and QEMU exits
- `power_reboot()` writes `0x7777` and QEMU restarts the machine
- Registered as device **`power0`** (no IRQ): writing `POWER_COMMAND_OFF` or `POWER_COMMAND_REBOOT` runs that command
- Ctrl-D in the echo loop sends `POWER_COMMAND_OFF` to `power0`

### Devices: `kernel/device.c`

Every driver fills in the same `device_t` struct:

```c
typedef struct device {
    const char *name;                          // "uart0", "power0"
    uint32_t irq;                              // DEVICE_NO_IRQ (0) if none
    int     (*init)(struct device *dev);
    void    (*interrupt)(struct device *dev);
    int64_t (*read)(struct device *dev, void *buffer, uint64_t length);
    int64_t (*write)(struct device *dev, const void *buffer, uint64_t length);
    uint64_t block_size;                       // block devices only
    uint64_t block_count;
    int     (*read_block)(struct device *dev, uint64_t block, void *buffer);
    int     (*write_block)(struct device *dev, uint64_t block, const void *buffer);
    int ready;
    int disabled;                              // disabled by the AI space after a crash
} device_t;
```

There are two kinds of devices, like on Linux and Windows:
- **Character devices** (`uart0`): a stream of bytes, used with `read` / `write`
- **Block devices** (`disk0`): numbered fixed-size blocks, used with `read_block` / `write_block`

| Function | What it does | Windows equivalent |
|---|---|---|
| `device_register(dev)` | Add a driver to the table (max 16, names must be unique) | Installing a driver |
| `device_init_all()` | Run every `init()`, enable its IRQ in the PLIC, log `Device ready` | Drivers starting at boot |
| `device_find(name)` | Look a device up by name | Opening `COM1` |
| `device_read()` / `device_write()` | The same call for every character device (`-1` if not ready or not supported) | `ReadFile()` / `WriteFile()` |
| `device_read_block()` / `device_write_block()` | Read or write one block of a block device (checks the block number is in range) | Sector reads by a storage driver |
| `device_handle_irq(irq)` | Run the interrupt handler of the device that owns `irq` | Interrupt dispatch to a driver's ISR |
| `devihow to check how much storage left on ubuntu\ce_list()` / `device_count()` | Print / count the table | Device Manager |
| `device_disable(dev)` | Stop using a driver (not ready, IRQ off), after an AI verdict | Disabling a device in Device Manager |

Every call into a driver is wrapped with `process_driver_enter()` / `process_driver_leave()`, so when a crash happens the kernel knows **which driver** was running. Drivers on the AI space's disabled list are skipped by `device_init_all()` after a warm restart.

Adding a new device means writing its driver file and calling its register function. The trap handler and the PLIC setup don't change, and the disk driver proved it.

### Processes and the AI-aware scheduler: `kernel/process.c`

**Processes** (kernel threads for now: they share the kernel's memory; user mode comes later):

| Field | Meaning |
|---|---|
| `pid`, `name` | Identity, like Task Manager |
| `process_class` | INTERACTIVE, AI_AGENT, NORMAL, BACKGROUND (or IDLE for pid 0) |
| `state` | READY, RUNNING, SLEEPING, EXITED |
| `context` | Saved `ra`, `sp`, `s0`–`s11` (used by `context_switch`) |
| `stack` | 16 KiB from `kmalloc` |
| `cpu_ticks`, `vruntime` | CPU time used, and weighted virtual runtime |

**Scheduling classes:**

| Class | Rule | Time slice | Meant for |
|---|---|---|---|
| **INTERACTIVE** | Always runs first (round-robin among themselves) | 1 tick | Keyboard, console, anything waiting for you |
| **AI_AGENT** | Weight **60** | 3 ticks | Agentic AI work: coding agents, LLM planning |
| **NORMAL** | Weight **30** | 2 ticks | Regular programs |
| **BACKGROUND** | Weight **10** | 1 tick | Small NNs, maintenance |
| IDLE | Only when nothing else is ready | | pid 0 (`kernel_main`), runs `wfi` |

**How the weighted share works (virtual runtime):** each tick a process runs, its `vruntime` grows by `600 / weight` (AI_AGENT +10, NORMAL +20, BACKGROUND +60). The scheduler always picks the READY weighted process with the **lowest** `vruntime`. So an AI agent can run 6 ticks for every 1 tick of background work, but everyone keeps moving forward, which means **no starvation**. A process that wakes up or is created starts at the current lowest `vruntime`, so it can't grab a huge burst. When only one process is busy, it gets 100% of the CPU. The weights and slices are in `process.h`.

**API:**

| Function | What it does | Windows equivalent |
|---|---|---|
| `process_create(name, class, entry, arg)` | Start a new process | `CreateThread` |
| `process_yield()` | Give up the CPU voluntarily | `SwitchToThread` |
| `process_sleep(ticks)` | Sleep for N ticks (10 ms each) | `Sleep` |
| `process_exit()` / `process_kill(pid)` | End a process | `ExitThread` / `TerminateThread` |
| `process_list()` | Print the process table | Task Manager |
| `scheduler_tick()` | Called on every timer tick: charge CPU time, wake sleepers, preempt | The clock interrupt handler |

**Current limits (fine for now, to fix later):** the kernel isn't fully preemption-safe yet (the heap has no locks, so `process_create` disables interrupts around it), the console polls every tick instead of waiting on an event, and a disk request waits with `wfi` instead of letting other processes run.

### AI space (Guardian core): `kernel/aispace.c` + `kernel/guardian.c`

```text
cores 0-3, 5-7: KnocOS kernel                core 4: AI space (M-mode, own memory)
  every tick: heartbeat++  ──── mailbox ────►  watches the heartbeat every 10 ms
  panic / trap: crash info ──── mailbox ────►  crash? → collect → diagnose → black box → act
  M-mode timer: last_kernel_pc ─ mailbox ───►  no heartbeat for 2 s? → freeze → same steps
```

**Protection:**
- The AI space lives at `0x90000000` (256 MiB): its code, data and stack are placed there by the linker script, followed by the clean copy of the kernel. The page allocator never hands out those pages
- **PMP** (set in M-mode on core 0 at boot) blocks the kernel from reading, writing or executing there. `make test` checks it: the kernel's probe read of `0x90000000` must fail with an access fault
- `aispace.c` is **self-contained**: its own UART output, clock reading and polled disk driver. It calls no kernel code. Its only link to the kernel is the mailbox (`nm -u kernel/aispace.o` shows just `guardian_mailbox`)

**The mailbox** (`kernel/mailbox.h`, its own linker section): the AI space resets it on startup. The kernel writes the heartbeat and uptime every tick, the current process on every switch, and crash details (`scause`, `sepc`, `stval`, `ra`, `sp`, message) on a trap or panic. Core 0's M-mode timer handler writes `last_kernel_pc` every tick, so even a frozen kernel reveals **where** it's stuck.

**What happens on a crash or freeze:**
1. **Detect:** `kernel_state = PANICKED` (crash), or no heartbeat for 2 s (freeze)
2. **Collect** the details from the mailbox
3. **Diagnose (rule brain, Tier 0):** null pointer, bad pointer, page fault in RAM, access to the protected AI space, illegal instruction, misaligned access, internal panic, or infinite loop with interrupts off
4. **Black box:** the AI space resets the disk and writes the report with its **own polled driver** (no interrupts, no kernel code needed). Last 8 sectors of the disk: 1 header sector (total crashes, reported crashes, crashes in a row) + 4 report slots
5. **Act:**

| Crashes in a row | Action |
|---|---|
| 1–2 | **Warm kernel restart**: only the kernel cores restart, from the clean copy |
| 3 | Warm kernel restart into **safe mode** (minimal services only) |
| 4+ | **Halt the kernel** (crash loop detected). The kernel cores stay stopped, the AI space stays online |

If the kernel cores can't be stopped (stuck in M-mode), the AI falls back to rebooting the whole machine as in v0.8.0.

6. **After the restart:** the kernel prints `Warm restart #N by the AI space` and every new black box report with the AI's diagnosis and action. A `guardian` background process resets the crash streak after 60 s without a crash

**Warm kernel restart, step by step:**

```text
boot    core 0 waits in boot.S (boot_request / boot_ack handshake) until
        core 4 has copied kernel_start..kernel_image_end into protected memory
crash   core 4 sets park_request and raises a machine software interrupt on every kernel core (CLINT MSIP)
        each core → machine_trap → aispace_park_core0() (AI space code) → spins, parked
        core 4 compares the kernel code with the clean copy, saves the black box,
        copies the clean kernel back, resets the mailbox, releases the cores
        every core → fence.i → _start → a fresh kernel. Core 4 never stopped
        (without park_request the same interrupt is only a wake-up signal between kernel cores)
```

**Fault containment (process crashes):**

```text
trap in a process, interrupts on  → [OOPS], process state CRASHED, schedule() away
guardian process (every 100 ms)   → posts the fault to the mailbox (fault_seq)
AI space                          → diagnoses, verdict: RESTART_PROCESS and/or DISABLE_DRIVER
guardian process                  → applies it: process_restart() / device_disable()
```

A process is restarted up to 3 times, then left stopped. With no AI space (`-smp 1`), the kernel applies the same rules itself after a 1 s timeout.

**Test keys** (in the console):

| Key | What it breaks | Expected result |
|---|---|---|
| **Ctrl-F** | The console process writes to an unmapped address | Contained, console restarted |
| **Ctrl-X** | The `faulty0` driver writes to an unmapped address | Contained, driver disabled, console restarted |
| **Ctrl-K** | Bad pointer with interrupts off (kernel code) | Kernel crash → warm restart |
| **Ctrl-O** | Overwrites `log_info()` with zeros, then calls it | Code check finds the damage → clean copy restored |
| **Ctrl-P** | `panic()` | Kernel crash → warm restart |
| **Ctrl-W** | Infinite loop with interrupts off | Freeze detected in 2 s → warm restart |

**Limits (next steps):** the brain is rules for now (a small NN in v0.15, an LLM later). The disabled-driver list lives in the AI space's memory, so a full power cycle clears it. The AI space and the kernel share the UART without a lock, so their output can mix if both print at once (spinlocks come in v0.10). All processes are still kernel threads, so a process bug can still corrupt kernel memory; real isolation comes with user mode (v0.11).

### User mode and system calls: `kernel/syscall.c`, `kernel/elf.c`, `user/`

```text
virtual address space of one program (its own page table)
0x1000000000  program code + data      (from the ELF file, R-X / RW-, U bit)
0x1100000000  mem_alloc() blocks       (grows up, megapages for blocks >= 2 MiB)
0x1F80000000  top of the 64 KiB stack  (grows down)
kernel, RAM, devices: mapped too, but WITHOUT the U bit → the program can't touch them
```

**Entering and leaving U-mode:**
- `user_enter()` (`switch.S`) sets `sepc` = program entry, `sstatus.SPP` = 0 (U-mode), `sscratch` = the process's kernel stack, clears every register and runs `sret`
- `sscratch` is 0 while in the kernel and holds the kernel stack while in a program. `supervisor_trap` swaps it with `sp` to know where the trap came from, so a program's stack pointer is never trusted
- On the way back, `sstatus.SPP` decides: `sret` to the program (after restoring `sscratch`) or to kernel code
- Each process has a `satp` value. The scheduler switches page tables on every process switch (`vm_switch()`)

**System calls** (`kernel/syscall_abi.h`, shared by kernel, AI space and programs): number in `a7`, arguments in `a0`–`a5`, `ecall`, result in `a0` (negative = error: `E_FAULT`, `E_PERM`, `E_NOMEM`...).

| Call | Capability | What it does |
|---|---|---|
| `exit(code)` | | End the program, free all its memory |
| `write(fd, buf, len)` / `read(fd, buf, len)` | `CONSOLE` (fd 0–2) | Screen output / keyboard input (waits for a key) or file data (fd 3+) |
| `getpid()`, `yield()`, `sleep(ticks)`, `uptime()` | | Process basics |
| `spawn(name)` | `SPAWN` | Start a built-in program |
| `mem_alloc(bytes)` | `MEMORY` | Get zeroed memory, within the quota |
| `open(path, flags)` | `FILES_READ` / `FILES_WRITE` | Open a file (`O_READ`, `O_WRITE`, `O_CREATE`, `O_TRUNC`), returns a file descriptor |
| `close(fd)`, `seek(fd, offset)` | | Close / move in an open file |
| `stat(path, out)`, `readdir(path, i, out)` | `FILES_READ` | Size and type / the i-th entry of a directory |
| `mkdir(path)`, `remove(path)` | `FILES_WRITE` | Create a directory / remove a file or empty directory |

**Safety:**
- `copy_from_user()` / `copy_to_user()` look up every page in **the program's own page table** (must be a user page with the right permission) and copy through its physical address. Bad pointers give `E_FAULT`
- Any exception in U-mode is contained: the program is stopped and reported to the AI space, whatever it was doing
- **Capabilities** per program; a denied call is refused, counted and logged `[SECURITY]`
- **Quotas:** 16 MiB for normal programs, 1 GiB for `AI_AGENT` programs. Memory is always zeroed before a program gets it

**Programs** (`user/`): `crt0.S` + `ulib.c` (system call wrappers, `print`) + one `.c` file each, linked at `0x1000000000` by `user/linker.ld`. Programs are loaded from `/bin/<name>` on the KnocFS disk. `kernel/programs.S` also embeds them in the kernel image (`.incbin`) as a fallback, and `kernel/program.c` gives each one a class and capabilities (the kernel decides permissions, not the file). The data segment is page-aligned, and `elf_load()` refuses segments that share a page. `elf_load()` checks the header and maps each `PT_LOAD` segment with its permissions.

| Program | Class, capabilities | Shows |
|---|---|---|
| `hello` | NORMAL, all | System calls work, memory, sleep, yield |
| `badcall` | NORMAL, CONSOLE + SPAWN | Kernel/unmapped pointers and unknown calls are refused |
| `noperm` | NORMAL, CONSOLE | A call without its capability is denied |
| `hog` | NORMAL, CONSOLE + MEMORY | The 16 MiB quota |
| `bigmem` | **AI_AGENT**, CONSOLE + MEMORY | An AI program gets 256 MiB |
| `crash` (Ctrl-U) | NORMAL, CONSOLE | Null pointer → AI restarts it 3 times, then stops it |
| `spy` (Ctrl-E) | NORMAL, CONSOLE | Forbidden call + reading kernel memory → blocked, AI won't restart it |
| `files` | NORMAL, CONSOLE + FILES | Note that survives reboots, read, list `/bin`, 20 KB write/read/remove |
| `modelcheck` | **AI_AGENT**, CONSOLE + FILES_READ + MEMORY | Loads the 8 MiB test model from `/models` and checks every byte |

### Disk: `kernel/virtio_blk.c`

A **virtual hard disk**: QEMU exposes the file `disk.img` on your PC as a virtio block device, like the `.vdi` file behind a VirtualBox disk.

- virtio-mmio slot 0 at `0x10001000`, IRQ 1, registered as block device **`disk0`** (512-byte sectors)
- `init`: checks the magic value (`"virt"`), version 2 and device ID 2 (block). Then the status handshake: `ACKNOWLEDGE` → `DRIVER` → feature negotiation (only `VIRTIO_F_VERSION_1`) → `FEATURES_OK` → queue setup → `DRIVER_OK`. Finally it reads the disk size from the config space
- **Virtqueue** (8 entries): three pages from `page_alloc()`, cleared to zero
  - **descriptor table:** where each buffer is and how long it is
  - **available ring:** the driver says "new request here"
  - **used ring:** the device says "request finished"
- Each request is a chain of 3 descriptors: **header** (read/write + sector number) → **data** (512 bytes) → **status** byte (written by the device, 0 = OK)
- Each request carries 1–8 sectors (`read_blocks` / `write_blocks`), so one 4 KiB filesystem block is one request
- The driver adds the chain to the available ring, **notifies** the device, and the process **sleeps on a wait queue** until the interrupt handler sees the used ring move and wakes it. Other processes run meanwhile
- A **sleep lock** lets one process use the disk at a time (the descriptors and buffer are shared)
- Data goes through a 4 KiB buffer inside the driver, so callers can pass any kernel buffer (including heap addresses, which aren't physical addresses)
- If no disk is attached, `init` fails, the boot log shows `Device failed: disk0`, and KnocOS keeps running

### Shell and terminal: `user/knocsh.c`, `kernel/tty.c`

```text
keyboard → UART interrupt → console process (kernel)
             Ctrl-D power off, Ctrl-C kill the foreground program, test keys
             everything else → tty_input() → tty buffer → read(0) in knocsh
knocsh → echoes the key, edits the line (Backspace), runs the command on Enter
```

- **`knocsh`** is a normal user program with every capability, including `SYSTEM`. It keeps its own current directory and resolves relative paths, `.` and `..` itself
- **Built-in commands** use system calls. Any other word is looked up in `/bin` and run as a program: the shell `spawn`s it and `wait`s for it (`&` runs it in the background)
- **`tty.c`**: a 256-byte input buffer with a wait queue. The program flagged `PROGRAM_TERMINAL` (the shell) becomes the terminal's owner when it starts or is restarted. While no shell is running, the console echoes keys itself
- **Ctrl-C:** while the shell waits for a program, that program is the **foreground** process, and Ctrl-C kills it
- **`kill`** from the shell only works on user programs (`E_PERM` for the kernel's own processes). A killed process has its sleep locks released, and `wait` returns `E_KILLED`

| System call | Returns |
|---|---|
| `wait(pid)` | The exit code, `E_KILLED` (Ctrl-C / kill) or `E_CRASHED` (the AI handles it) |
| `ps(i, info)` | The i-th process: pid, name, class, state, CPU ticks, memory, user or kernel |
| `kill(pid)` | Stops a user program |
| `sysinfo(info)` | RAM, disk, uptime, AI space status, warm restarts, crash counts, disabled drivers, safe mode |
| `devinfo(i, info)` | The i-th device: name, IRQ, ready / disabled by the AI |
| `crashinfo(i, info)` | The i-th newest black box report: type, process, driver, message, AI diagnosis and action |

All except `wait` need the `SYSTEM` capability.

### Wait queues: `kernel/process.c`

- `process_block(channel, timeout)` puts the current process to sleep (state `BLOCKED`) until `process_wake(channel)`, or until the timeout. A channel is just an address: the keyboard buffer, the disk, a process
- The caller checks its condition and blocks **with interrupts off**, so a wake-up can't slip in between the check and the sleep
- Interrupt handlers call `process_wake()`. If the woken process is interactive (or the CPU was idle), `scheduler_preempt()` switches to it right after the interrupt
- **Sleep locks** (`sleeplock_acquire` / `release`) are locks a process can hold while it sleeps (disk, filesystem). A process that crashes has its sleep locks released
- Users: keyboard (`uart_wait_input()`), disk requests, `process_wait()`, the guardian (woken at once by a crash)

### Filesystem: `kernel/knocfs.c`, `tools/knocfs.py`

```text
disk0 sectors:  [0..2047] boot test data │ KnocFS │ last 8: AI black box
KnocFS blocks:  superblock │ free-block bitmap │ inode table (1024 × 128 B) │ data
```

- **Inode:** type (file/dir), size, and up to **12 extents** `{start block, count}`. A file written in one piece is one extent, so reading it is one contiguous run
- **Directory:** a file of 64-byte entries `{inode, name}`. Paths are absolute (`/models/test-model.bin`), names up to 59 characters
- **Allocation:** a file grows in place when the next blocks are free, otherwise it gets the first run of free blocks that is long enough (or the longest one). New blocks are zeroed, so old data never shows up in a file
- **Locking:** one sleep lock for the whole filesystem (operations sleep during disk I/O)
- **API:** `knocfs_mount`, `lookup`, `create`, `remove`, `stat`, `readdir`, `truncate`, `read`, `write`, `usage`
- **Host tool:** `tools/knocfs.py` writes the same layout from Linux: `format`, `mkdir`, `put`, `put-text`, `ls`, `cat`, `rm`, `info`, `make-test-model`. `scripts/mkdisk.sh` builds the default disk (64 MiB: `/bin` programs, `/hello.txt`, `/models/test-model.bin`, `/home`, `/tmp`)

**Limits:** no caching yet (every access reads the disk), a file can have at most 12 extents, files have no timestamps or owners, and the whole filesystem has one lock.

### Early memory info: `kernel/memory.c`

Helpers from the first milestone: total RAM (B/KB/MB/GB), kernel size, stack size, used/free bytes. These are not called by `kernel_main` right now; the page allocator does the real accounting.

---

## Building

Requirements: `riscv64-unknown-elf-gcc`, `riscv64-unknown-elf-ld`, `qemu-system-riscv64`, `python3` (disk tool and inspection targets).

`make run` first copies the freshly built programs into `/bin` on `disk.img` (other files stay). A `disk.img` from before v0.12 has no filesystem: run `make reset-disk` once.

```bash
make            # build knocos.elf
make run        # boot KnocOS in QEMU (power off: Ctrl-D, force quit: Ctrl-A then X)
make test       # boot twice with a fresh test disk, run every self-test, print PASS/FAIL
make clean      # remove build artifacts (keeps disk.img)
make reset-disk # recreate disk.img: 64 MiB KnocFS with /bin, /models, /home, /tmp (DISK_MB=4096 for a bigger one)
make put FILE=x DEST=/models/x  # copy a file from your PC onto the disk (for example a model)
make ls DIR=/models             # list a directory on the disk
make size       # kernel / stack size info
make pages      # physical page layout summary
make timer-test # run the standalone mtime printer (timer.elf)
```

Compiler flags:

```text
-march=rv64g -mabi=lp64d -mcmodel=medany
-ffreestanding -fno-pie -fno-pic -nostdlib -nostartfiles -nodefaultlibs
-Wall -Wextra -Werror        # every warning is an error
-MMD -MP                     # automatic header dependencies
-DKNOCOS_VERSION='"v0.15.0"'  # from the VERSION file
```

Header files and `boot/linker.ld` are tracked automatically, so `make` always rebuilds what changed.

### Testing

`make test` runs `scripts/test.sh`, which:

1. Creates a fresh temporary disk image with `scripts/mkdisk.sh` (`Hello from the host!` in sector 0, a KnocFS filesystem with the programs and the test model)
2. **Boot 1:** types a shell session (`ls`, `cat`, `echo > file`, `cd`, `pwd`, `ps`, `mem`, `devices`, `ai`, `crashes`, `counter` + Ctrl-C, `kill`) and checks the output, and checks that every self-test message appears (including the device tree with 2048 MiB and 8 CPUs, the buddy allocator, 1026 megapages, the 1 GiB block test, the spinlock test, the disk tests, `Disk boot count: 1`, and the 5 user programs with the no-leak check) and there is no `[PANIC]`
3. The first typed line, `knocos-echo-test`, must come back as `unknown command` from the shell (this tests the UART → PLIC → trap → console → tty → shell path)
4. Presses Ctrl-D and checks that KnocOS powers QEMU off within 15 seconds
5. **Boot 2** with the same disk image: checks `Disk boot count: 2` and that the `files` program finds the note it wrote on Boot 1, which proves sectors and files survive a reboot
6. **Run 3** (user programs, fault containment and warm restarts), on a fresh disk in one QEMU session: Ctrl-U → the `crash` program is restarted 3 times by the AI, then left stopped. Ctrl-E → `spy`'s forbidden call is logged, its read of kernel memory is blocked, and the AI refuses to restart it (the test fails if `spy` ever reads kernel memory). Ctrl-F → the console crash is contained and the AI restarts it. Ctrl-X → the AI disables `faulty0` and restarts the console, and a second Ctrl-X does nothing. Ctrl-W (freeze) → warm restart #1. Ctrl-O (code corruption) → the code check finds it, warm restart #2 from the clean copy. Ctrl-K (3rd kernel crash in a row) → warm restart #3 into **SAFE MODE**. The test fails if the machine was rebooted instead, or if a kernel line and an AI space line were ever mixed
7. **Run 4**, same disk: one more panic is the 4th crash in a row → the kernel stays halted and the AI space says it stays online

QEMU runs with `-smp 8 -m 2G` (cores 0-3 = kernel, core 4 = AI space, cores 5-7 = AI programs, 2 GiB of RAM). `make run RAM=8G` runs with more memory (at least `1G`).

The disk used by `make run` is `disk.img` in the project folder. It isn't deleted by `make clean`, so its data persists between runs.

```text
  ok   Supervisor timer interrupts verified
  ok   Supervisor trap handler verified
  ...
  ok   Powering off
RESULT: PASS
```

The same test runs on GitHub Actions for every push (`.github/workflows/ci.yml`).

### Versioning

The version lives in `VERSION` and follows [Semantic Versioning](https://semver.org/). Below `1.0.0`, each minor version is a development milestone. Changes are recorded in `CHANGELOG.md`.

---

## Debugging

Start QEMU paused with a GDB server:

```bash
env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin \
qemu-system-riscv64 -machine virt -bios none \
  -kernel knocos.elf -nographic -S -gdb tcp::1234
```

Connect:

```bash
gdb-multiarch knocos.elf
(gdb) target remote :1234
(gdb) info registers pc sp
(gdb) stepi
(gdb) p/x $mcause
(gdb) p/x $satp
```

Inspect the ELF:

```bash
riscv64-unknown-elf-readelf -S knocos.elf   # sections
riscv64-unknown-elf-readelf -l knocos.elf   # segments (R-X / RW-)
riscv64-unknown-elf-nm -n knocos.elf        # symbols
riscv64-unknown-elf-objdump -d knocos.elf   # disassembly
```

---

## Roadmap

### Phase 1: Boot ✅

- [x] RISC-V kernel entry
- [x] Linker script
- [x] Stack initialization
- [x] Boot into QEMU
- [x] C kernel entry

### Phase 2: Kernel Basics ✅

- [x] UART output
- [x] Basic kernel structure
- [x] Memory layout symbols
- [x] Basic memory information
- [x] Kernel logging (`log_info` / `log_warn`)
- [x] Panic handler

### Phase 3: Memory Management ✅

- [x] Basic RAM model
- [x] Kernel and stack boundaries
- [x] Page alignment
- [x] Physical page allocator (bitmap, replaced by a buddy allocator in v0.10.0)
- [x] Device tree: RAM size from the firmware
- [x] 2 MiB megapages
- [x] Spinlocks
- [x] Page tracking and debug map
- [x] Virtual memory
- [x] RISC-V Sv39 page tables, paging enabled
- [x] Kernel heap
- [x] `kmalloc` / `kfree`
- [x] Block splitting and coalescing
- [x] Automatic heap growth
- [x] Separate R-X / RW- ELF segments

### Phase 4: Hardware & Interrupts ✅

- [x] Timer (CLINT `mtime` / `mtimecmp`)
- [x] Machine-mode trap entry and register save/restore
- [x] Timer interrupt handling and tick counter
- [x] Reliable timer self-test
- [x] Supervisor-mode trap handling in C
- [x] Exception decoding (page faults, illegal instruction, ...)
- [x] Timer interrupts forwarded to Supervisor mode
- [x] Interrupt controller (PLIC)
- [x] UART driver with input
- [x] Power-off / reboot driver
- [x] Device abstraction (device table, `uart0`, `power0`)

### Phase 5: Processes & Scheduling

- [x] Process structure
- [x] Context switching
- [x] Scheduler (timer-driven preemption)
- [x] AI-aware scheduling classes (INTERACTIVE, AI_AGENT, NORMAL, BACKGROUND)
- [x] Multiple processes
- [x] Fault containment (a crashing process only stops itself)
- [x] Event-based waiting (wait queues, no polling)
- [x] User mode
- [x] System calls

### Phase 6: Storage & Filesystems

- [x] Block device (virtio-blk)
- [x] Disk driver (`disk0`, interrupt-driven sector read/write)
- [x] Filesystem (KnocFS, extents)
- [x] File descriptors
- [ ] VFS layer (more than one filesystem type)

### Phase 7: User Space

- [x] User programs (built in, ELF loader)
- [x] Shell (`knocsh`)
- [ ] Standard library
- [x] Process management (`ps`, `kill`, `wait`, Ctrl-C)

### Future Direction

Long-term ideas (not part of the current implementation):

- AI-native operating-system concepts
- Local AI runtimes
- Hardware acceleration and custom AI accelerators
- AI-specific scheduling and AI-aware memory management
- Custom RISC-V hardware / FPGA-based CPU experimentation

---

## Milestone Roadmap

The detailed plan is in [`goal.md`](goal.md) (section 4b).

| Phase | Versions | Goal |
|---|---|---|
| A. AI survives crashes | v0.8 – v0.9 | AI space on its own core with protected memory, black box, watchdog, warm kernel restart |
| B. Real OS foundation | v0.10 – v0.13 | Big memory, user mode, filesystem, shell |
| C. First real AI | v0.14 – v0.15 | Small NN runtime, crash classifier and intent classifier NNs |
| D. LLM agent | v0.16 – v0.19 | C library, llama.cpp port (Qwen), AI memory layer, agent + tools |
| E. Connected | v0.20 | Networking, model download, KnocNet |
| F. Smooth GUI (last) | after v0.20 | Framebuffer, mouse/keyboard input, window system, desktop with an AI panel |
| → v1.0 | | Real hardware, speed, app compatibility |

---

## Road to Production

KnocOS aims to become a production-grade operating system. This is what that requires and where it stands today.

### Engineering quality

| Requirement | Status |
|---|---|
| Builds with zero warnings (`-Wall -Wextra -Werror`) | ✅ |
| Correct incremental builds (automatic dependencies) | ✅ |
| Automated boot test (`make test`) | ✅ |
| Continuous integration on every push | ✅ |
| Versioning and release notes | ✅ |
| Unit tests for individual subsystems (heap, pages, page tables) | ⬜ |
| Stress / fuzz testing | ⬜ |
| Kernel debug tooling (backtraces, assertions, memory leak checks) | ⬜ |

### Kernel features

| Requirement | Status |
|---|---|
| Memory management (pages, paging, heap) | ✅ |
| Interrupts and exception handling | ✅ |
| Device driver model (device abstraction) | ✅ |
| Processes, scheduler, context switching | ✅ (kernel threads) |
| AI-aware scheduling | ✅ |
| User mode and memory isolation between programs | ✅ |
| System calls | ✅ (9 calls, capabilities, quotas) |
| Multi-core (SMP) support and locking | 🚧 (spinlocks, shared UART lock; the kernel itself runs on one core) |
| Disk driver (virtio-blk) | ✅ |
| Filesystem | ✅ (KnocFS) |
| Networking stack | ⬜ |
| Graphics and input devices | ⬜ |

### Reliability and security

| Requirement | Status |
|---|---|
| Kernel pages with correct permissions (no `RWX` mappings) | ⬜ |
| Stack overflow protection (guard pages) | ⬜ |
| Permission / capability system | ⬜ |
| Crash detection, black box and automatic recovery (AI space on its own core) | ✅ |
| Fault containment (no single fault takes down the whole OS) | ⬜ |
| Secure boot / update path | ⬜ |

### Platform

| Requirement | Status |
|---|---|
| Runs on QEMU `virt` | ✅ |
| Device tree parsing (no hard-coded addresses) | ⬜ |
| Runs on real RISC-V hardware (with OpenSBI) | ⬜ |
| Other architectures (x86-64 / ARM64) | ⬜ |

---

## Philosophy

KnocOS is being built from the lowest practical level upward:

```text
Hardware → RISC-V ISA → Boot code → Linker → Kernel → Memory management
→ Interrupts → Processes → System calls → Filesystem → User space → Applications
```

The goal is not simply to produce an operating system. The goal is to understand **why every layer exists and how the layers communicate with each other.**

---

## Status

**Early development (v0.15.0):** being built toward a production-grade OS. Not yet ready for real-world use.

Boot, logging, physical memory, Sv39 paging and the kernel heap are working. Timer interrupts (forwarded to the kernel) and Supervisor-mode exception handling are working. The PLIC and an interrupt-driven UART driver are in, so KnocOS now reacts to the keyboard. A power-off driver, automated tests and CI are in place. Device abstraction is done, so **Phase 4 is complete**, and a virtio-blk disk driver gives KnocOS permanent storage. Processes and an AI-aware scheduler now let several tasks run at once, with AI agent work getting the largest CPU share. An AI space on its own CPU core, protected by hardware, survives kernel crashes and freezes, diagnoses them and recovers. A crashing process now only stops itself, and the AI space decides the fix; a crashing kernel is restarted from a clean copy while the AI keeps running. KnocOS now reads its RAM size from the device tree and manages gigabytes of memory with a buddy allocator and megapages. Programs run in user mode with their own page tables and talk to the kernel only through checked system calls. Processes sleep on wait queues instead of polling, and KnocFS stores programs, files and AI model files on the disk. The shell `knocsh` lets you use it: files, programs, processes, and the AI's crash reports.
