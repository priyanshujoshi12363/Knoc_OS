
# KnocOS

[![CI](https://github.com/priyanshujoshi12363/Knoc_OS/actions/workflows/ci.yml/badge.svg)](https://github.com/priyanshujoshi12363/Knoc_OS/actions/workflows/ci.yml)
![Version](https://img.shields.io/badge/version-v0.9.0-blue)
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
- Interactive **keyboard echo** (Enter = new line, Backspace erases)
- Kernel logging (`log_info`, `log_warn`, `log_trap`) and a `panic` handler
- **Physical page allocator** (bitmap, 4 KiB pages)
- **Sv39 virtual memory** (3-level page tables, identity-mapped kernel, paging enabled via `satp`)
- **Kernel heap** (`kmalloc` / `kfree`) in its own virtual region with block splitting, coalescing and automatic page growth
- **Timer interrupts at 100 Hz**: M-mode catches the hardware timer and forwards each tick to the kernel (S-mode), which counts it. Time is read with `rdtime`
- Machine-mode trap handler that saves/restores all registers and prints `mcause` for unhandled traps
- **Supervisor-mode trap handler in C**: exceptions are delegated to S-mode, decoded by name and reported with `scause` / `sepc` / `stval`
- Standalone `timer/` test program for reading `mtime`
- **Power-off / reboot** driver (QEMU test device): press **Ctrl-D** to shut KnocOS down
- **virtio-blk disk driver** (`disk0`): reads and writes 512-byte sectors of `disk.img` through interrupts, so data survives reboots
- **AI space (Guardian core)**: CPU core 1 runs a protected space the kernel can't touch (PMP hardware). It watches the kernel, and when the kernel crashes or freezes it **keeps running**: it diagnoses the problem, saves a crash report (black box) to disk, and **restarts only the kernel from a clean copy** (warm restart), so the AI itself never stops
- **Fault containment**: a crashing process is stopped alone, like a Linux "oops". The AI space diagnoses it and decides the fix: restart the process, leave it stopped if it keeps crashing, or **disable the driver** the crash happened in
- **Processes and an AI-aware scheduler**: kernel processes with their own stacks, context switching, timer preemption, and 4 scheduling classes where **AI agent work gets the largest CPU share** (60%) while the keyboard stays instant and nothing starves
- **Automated tests** (`make test`) and **GitHub Actions CI** on every push
- Make-based build with automatic header dependencies and `-Wall -Wextra -Werror`

### Boot output

```text
[INFO] KnocOS v0.9.0 starting
[INFO] Supervisor interrupts enabled
[INFO] Page memory initialized
[INFO] Virtual memory initialized
[INFO] Kernel page tables ready
[INFO] Kernel heap mapping prepared
[INFO] Enabling Sv39
[INFO] Sv39 enabled
[INFO] Kernel heap activated
[INFO] PMP verified: the kernel cannot read the AI space
[INFO] AI space online (core 1, 16 MiB protected at 0x0000000087000000)
[INFO] Allocation A successful
...
[INFO] Kernel heap 4.0 stress test passed
[INFO] Supervisor timer interrupts verified
[TRAP] Breakpoint
[TRAP] sepc   = 0x0000000080001308
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
[INFO] All self-tests passed
[INFO] Keyboard echo ready, start typing (Ctrl-D power off)
[INFO] Test keys: Ctrl-F process fault, Ctrl-X driver fault, Ctrl-K kernel fault, Ctrl-O overwrite kernel code, Ctrl-P panic, Ctrl-W freeze
hello knocos          ← what you type is echoed back
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

## ✅ Just Completed: Fault Containment + Warm Kernel Restart (v0.9.0)

**The AI never stops.** In v0.8.0 the AI space survived a kernel crash, but its only fix was rebooting the whole machine, which restarted the AI too. Now:

- **A crashing process only kills itself.** The kernel stops that process and keeps running. The AI space diagnoses the crash and sends back a verdict: restart it, leave it stopped (after 3 crashes), or disable the driver it crashed in.
- **A crashing kernel is restarted by the AI, not rebooted.** At boot the AI space copies the clean kernel into its protected memory. On a crash or freeze it stops core 0, checks the kernel code against the clean copy, saves the black box, restores the kernel and restarts **only core 0**. Core 1 keeps running the whole time.

```text
[INFO] Test driver fault (Ctrl-X): the faulty0 driver writes to an unmapped address
[OOPS] Store page fault in process console (pid 7) inside driver faulty0: stopping only this process
[AI] Process crash contained: console (pid 7), the kernel keeps running
[AI] Diagnosis: Bad pointer: the code accessed an address that is not mapped (stval).
[AI] Action: disable driver faulty0 (the crash happened inside it), restart console
[INFO] Driver faulty0 disabled (AI verdict)
[INFO] Process console restarted as pid 8, restart #2 (AI verdict)

[INFO] Test code corruption (Ctrl-O): overwriting log_info() with zeros, then calling it
[TRAP] Illegal instruction
[PANIC] Unhandled supervisor trap
[AI] Kernel crash detected: TRAP - Unhandled supervisor trap
[AI] Core 0 stopped: the kernel is paused while the AI works
[AI] Kernel code check: 14 bytes differ from the clean copy (code corrupted)
[AI] Diagnosis: Kernel code was overwritten in memory: a bad pointer wrote over it. The clean copy fixes it.
[AI] Black box saved (crash #2, 2 in a row)
[AI] Action: warm kernel restart (only core 0, the AI keeps running)
[AI] Restoring the kernel from its clean copy (38 KiB) and restarting core 0, restart #2
[INFO] KnocOS v0.9.0 starting
[INFO] AI space online (core 1, 16 MiB protected at 0x0000000087000000)
[INFO] Warm restart #2 by the AI space: kernel restored from a clean copy, the AI kept running (AI uptime 18049 ms)
[WARN] Device disabled by the AI space: faulty0 (it crashed before)
```

The "brain" is still rules (Tier 0). The small NN (v0.15) and later an LLM plug into the same place. **Next up: v0.10.0, big memory** (RAM size from the device tree, megapages, spinlocks for 2 cores) to make room for real AI models.

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
| *(uncommitted)* | Fault containment, AI verdicts (restart process / disable driver), warm kernel restart from a clean copy, kernel code check, version `v0.9.0` |

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
- **Console as a process:** the keyboard echo runs as an INTERACTIVE process that sleeps 1 tick when there's no input
- M-mode has its **own stack** (`mscratch`), so the timer handler never touches process stacks. The S-mode trap handler saves `sepc`/`sstatus` so a process switch inside a trap returns to the right place
- Self-tests: the disk test writes and reads back a sector, reads text placed in `disk.img` by the host, and increments a boot counter stored on the disk
- Self-tests: the timer test waits for 5 kernel ticks (1 s timeout, and they must not arrive faster than 10 ms apart), and the trap test runs `ebreak` and checks the handler ran and returned

Next steps:

- [x] v0.9.0: fault containment (a crashing process only kills itself), process auto-restart, disabling a crashing driver, warm kernel restart by the AI space
- [ ] v0.10.0: big memory (2–4 GiB+) for AI models
- [ ] v0.11.0+: user mode, filesystem, shell (see the Milestone Roadmap)

---

## Architecture

```text
Architecture: RISC-V 64-bit
ISA:          RV64G
ABI:          LP64D
Machine:      QEMU virt
Kernel base:  0x80000000
RAM:          128 MiB (0x80000000 – 0x88000000)
Cores:        2 (core 0 = kernel, core 1 = AI space)
Page size:    4 KiB
Paging:       Sv39
```

### Privilege flow

```text
QEMU reset
   │
   ▼
_start (Machine mode)          boot/boot.S
   ├─ core 1? → aispace_boot → aispace_main() (M-mode, protected memory, never returns)
   ├─ core 0: clear .bss, set stack pointer
   ├─ PMP: block the AI space (0x87000000, 16 MiB), allow everything else
   ├─ satp = 0 (paging off), clear leftover interrupt state (a warm reboot keeps old CSRs)
   ├─ mtvec = machine_trap
   ├─ stvec = supervisor_trap
   ├─ medeleg: exceptions → S-mode
   ├─ mideleg: supervisor software + external interrupts → S-mode
   ├─ mstatus.MPP = S, MPIE = 1
   ├─ mcounteren.TM = 1 (rdtime)
   ├─ mtimecmp = mtime + TIMER_INTERVAL
   ├─ mie.MTIE = 1
   └─ mret
   │
   ▼
kernel_main (Supervisor mode)  kernel/main.c
   ├─ trap_enable_interrupts()  sie.SSIE + sie.SEIE + sstatus.SIE
   ├─ page_init()     physical page allocator
   ├─ vm_init()       build Sv39 page tables
   ├─ heap_init()     map first heap page
   ├─ vm_enable()     write satp, sfence.vma
   ├─ heap_activate() create first heap block
   ├─ heap stress test
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
                    start "console" (INTERACTIVE) → keyboard echo

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
     │  .data + .bss             (RW-)  incl. page bitmap
     ├──────────────────────── kernel_end
     │  Kernel stack (16 KiB)
     ├──────────────────────── stack_top
     │  (page aligned)
     │  Free pages → page_alloc()
     │    page tables, heap pages, ...
0x87000000 ─────────────── AI SPACE (16 MiB)   ← PMP: the kernel cannot read, write or execute here
     │  AI space code (R-X) and data (RW-)
     │  AI space stack (16 KiB)
     │  (rest reserved for future AI runtimes and models)
0x88000000 ─────────────── RAM END
```

(Exact addresses of `kernel_end` / `stack_top` change as the kernel grows. Use `make size` or `nm` to see them.)

### Virtual memory layout (Sv39)

| Virtual range | Maps to | Flags | Purpose |
|---|---|---|---|
| `0x80000000 – 0x88000000` | same (identity) | `R W X` | Kernel + all RAM |
| `0x0C000000 – 0x0C400000` | same (identity) | `R W` | PLIC |
| `0x10000000` (1 page) | same (identity) | `R W` | UART |
| `0x10001000` (1 page) | same (identity) | `R W` | virtio-blk (slot 0) |
| `0x90000000 – 0xA0000000` | pages from `page_alloc()` | `R W` | Kernel heap (grows on demand) |

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
│   ├── page.c/h      # Bitmap physical page allocator
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
│   ├── aispace.c/h   # AI space on core 1: watch, diagnose, black box, recover (self-contained)
│   ├── guardian.c/h  # Kernel side of the Guardian: mailbox, heartbeat, crash reporting, boot report, AI verdicts
│   ├── faulty.c/h    # faulty0: a test driver with a bug on purpose (Ctrl-X)
│   ├── mailbox.h     # Shared kernel ↔ AI space mailbox layout
│   └── blackbox.h    # On-disk crash report format
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

- Checks `mhartid`: **core 1 jumps to `aispace_boot`** (its own stack, then `aispace_main`), and any other extra core parks
- Core 0 **clears `.bss`** (a warm reboot doesn't clear memory) and sets `sp` to `stack_top`
- **PMP:** entry 0 blocks the AI space region (`0x87000000`, 16 MiB, no permissions), entry 1 allows all other memory. The kernel (S-mode) cannot change PMP, so it can never reach the AI space
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

### Physical pages: `kernel/page.c`

- 32768 pages × 4 KiB = 128 MiB, tracked by a 4 KiB bitmap (1 bit per page)
- Pages below the page-aligned `stack_top` (kernel + stack) are permanently reserved
- `page_alloc()`: first-fit scan, returns a physical page address or `0`
- `page_free()`: validates range, alignment, reserved region and double free
- `page_total()`, `page_used()`, `page_free_count()`, `page_debug()` (prints a USED/FREE run-length memory map)

### Virtual memory: `kernel/vm.c`

- Sv39 3-level page tables (512 × 8-byte PTEs per table), tables allocated with `page_alloc()`
- `vm_map()` walks VPN[2] → VPN[1] → VPN[0] and creates intermediate tables on demand
- `vm_map_range()` maps a range page by page
- `vm_init()` identity-maps all RAM (`RWX`), the UART (`RW`) and the PLIC (`RW`)
- `vm_enable()` writes `satp` (mode 8 = Sv39) and flushes the TLB with `sfence.vma`
- `vm_debug(va)` prints the full page-table walk for an address

### Kernel heap: `kernel/heap.c`

- Virtual heap region `0x90000000 – 0xA0000000`
- Linked list of blocks with header `{ size, free, next }`, 8-byte aligned allocations
- **First-fit** allocation
- **Splitting**: large free blocks are split to fit the request
- **Coalescing**: adjacent free blocks merge on `kfree`
- **Automatic growth**: when no block fits, a new physical page is allocated and mapped at `heap_end`, and the last free block is extended
- `kfree` ignores `NULL`, pointers not in the heap and double frees
- Two-phase init: `heap_init()` maps the first page *before* paging, and `heap_activate()` writes the first block header *after* Sv39 is on (`0x90000000` is only reachable through the page tables)

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
| `device_list()` / `device_count()` | Print / count the table | Device Manager |
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
core 0: KnocOS kernel                        core 1: AI space (M-mode, own memory)
  every tick: heartbeat++  ──── mailbox ────►  watches the heartbeat every 10 ms
  panic / trap: crash info ──── mailbox ────►  crash? → collect → diagnose → black box → act
  M-mode timer: last_kernel_pc ─ mailbox ───►  no heartbeat for 2 s? → freeze → same steps
```

**Protection:**
- The AI space lives at `0x87000000` (16 MiB): its code, data and stack are placed there by the linker script. The page allocator never hands out those pages
- **PMP** (set in M-mode on core 0 at boot) blocks the kernel from reading, writing or executing there. `make test` checks it: the kernel's probe read of `0x87000000` must fail with an access fault
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
| 1–2 | **Warm kernel restart**: only core 0 restarts, from the clean copy |
| 3 | Warm kernel restart into **safe mode** (minimal services only) |
| 4+ | **Halt the kernel** (crash loop detected). Core 0 stays stopped, the AI space stays online |

If core 0 can't be stopped (stuck in M-mode), the AI falls back to rebooting the whole machine as in v0.8.0.

6. **After the restart:** the kernel prints `Warm restart #N by the AI space` and every new black box report with the AI's diagnosis and action. A `guardian` background process resets the crash streak after 60 s without a crash

**Warm kernel restart, step by step:**

```text
boot    core 0 waits in boot.S (boot_request / boot_ack handshake) until
        core 1 has copied kernel_start..kernel_image_end (38 KiB) into protected memory
crash   core 1 raises a machine software interrupt on core 0 (CLINT MSIP)
        core 0 → machine_trap → aispace_park_core0() (AI space code) → spins, parked
        core 1 compares the kernel code with the clean copy, saves the black box,
        copies the clean kernel back, resets the mailbox, releases core 0
        core 0 → fence.i → _start → a fresh kernel. Core 1 never stopped
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

### Disk: `kernel/virtio_blk.c`

A **virtual hard disk**: QEMU exposes the file `disk.img` on your PC as a virtio block device, like the `.vdi` file behind a VirtualBox disk.

- virtio-mmio slot 0 at `0x10001000`, IRQ 1, registered as block device **`disk0`** (512-byte sectors)
- `init`: checks the magic value (`"virt"`), version 2 and device ID 2 (block). Then the status handshake: `ACKNOWLEDGE` → `DRIVER` → feature negotiation (only `VIRTIO_F_VERSION_1`) → `FEATURES_OK` → queue setup → `DRIVER_OK`. Finally it reads the disk size from the config space
- **Virtqueue** (8 entries): three pages from `page_alloc()`, cleared to zero
  - **descriptor table:** where each buffer is and how long it is
  - **available ring:** the driver says "new request here"
  - **used ring:** the device says "request finished"
- Each request is a chain of 3 descriptors: **header** (read/write + sector number) → **data** (512 bytes) → **status** byte (written by the device, 0 = OK)
- The driver adds the chain to the available ring, **notifies** the device, and sleeps with `wfi` until the interrupt handler sees the used ring move
- Data goes through a sector buffer inside the driver, so callers can pass any kernel buffer (including heap addresses, which aren't physical addresses)
- If no disk is attached, `init` fails, the boot log shows `Device failed: disk0`, and KnocOS keeps running

### Early memory info: `kernel/memory.c`

Helpers from the first milestone: total RAM (B/KB/MB/GB), kernel size, stack size, used/free bytes. These are not called by `kernel_main` right now; the page allocator does the real accounting.

---

## Building

Requirements: `riscv64-unknown-elf-gcc`, `riscv64-unknown-elf-ld`, `qemu-system-riscv64`, `python3` (for inspection targets).

```bash
make            # build knocos.elf
make run        # boot KnocOS in QEMU (power off: Ctrl-D, force quit: Ctrl-A then X)
make test       # boot twice with a fresh test disk, run every self-test, print PASS/FAIL
make clean      # remove build artifacts (keeps disk.img)
make reset-disk # recreate disk.img (1 MiB, "Hello from the host!" in sector 0)
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
-DKNOCOS_VERSION='"v0.9.0"'  # from the VERSION file
```

Header files and `boot/linker.ld` are tracked automatically, so `make` always rebuilds what changed.

### Testing

`make test` runs `scripts/test.sh`, which:

1. Creates a fresh temporary disk image with `Hello from the host!` in sector 0
2. **Boot 1:** checks that every self-test message appears (including the disk tests and `Disk boot count: 1`) and there is no `[PANIC]`
3. Types `knocos-echo-test` + Enter and checks the echo (this tests the UART → PLIC → trap path)
4. Presses Ctrl-D and checks that KnocOS powers QEMU off within 15 seconds
5. **Boot 2** with the same disk image: checks `Disk boot count: 2`, which proves data written to the disk survives a reboot
6. **Run 3** (fault containment and warm restarts), on a fresh disk in one QEMU session: Ctrl-F → the console crash is contained and the AI restarts it. Ctrl-X → the AI disables `faulty0` and restarts the console, and a second Ctrl-X does nothing. Ctrl-W (freeze) → warm restart #1. Ctrl-O (code corruption) → the code check finds it, warm restart #2 from the clean copy. Ctrl-K (3rd kernel crash in a row) → warm restart #3 into **SAFE MODE**. The test fails if the machine was rebooted instead
7. **Run 4**, same disk: one more panic is the 4th crash in a row → the kernel stays halted and the AI space says it stays online

QEMU runs with `-smp 2` (core 0 = kernel, core 1 = AI space).

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
- [x] Physical page allocator (bitmap)
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
- [ ] Event-based waiting (no polling)
- [ ] User mode
- [ ] System calls

### Phase 6: Storage & Filesystems

- [x] Block device (virtio-blk)
- [x] Disk driver (`disk0`, interrupt-driven sector read/write)
- [ ] Filesystem
- [ ] File descriptors
- [ ] VFS layer

### Phase 7: User Space

- [ ] User programs
- [ ] Shell
- [ ] Standard library
- [ ] Process management

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
| User mode and memory isolation between programs | ⬜ |
| System calls | ⬜ |
| Multi-core (SMP) support and locking | ⬜ |
| Disk driver (virtio-blk) | ✅ |
| Filesystem | ⬜ |
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

**Early development (v0.9.0):** being built toward a production-grade OS. Not yet ready for real-world use.

Boot, logging, physical memory, Sv39 paging and the kernel heap are working. Timer interrupts (forwarded to the kernel) and Supervisor-mode exception handling are working. The PLIC and an interrupt-driven UART driver are in, so KnocOS now reacts to the keyboard. A power-off driver, automated tests and CI are in place. Device abstraction is done, so **Phase 4 is complete**, and a virtio-blk disk driver gives KnocOS permanent storage. Processes and an AI-aware scheduler now let several tasks run at once, with AI agent work getting the largest CPU share. An AI space on its own CPU core, protected by hardware, survives kernel crashes and freezes, diagnoses them and recovers. A crashing process now only stops itself, and the AI space decides the fix; a crashing kernel is restarted from a clean copy while the AI keeps running.
