
# KnocOS

**KnocOS** is a from-scratch experimental operating system being built for learning and exploring low-level computer architecture, operating systems, memory management, and eventually AI-oriented systems.

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
- UART output through memory-mapped I/O
- Kernel logging (`log_info`, `log_warn`, `log_trap`) and a `panic` handler
- **Physical page allocator** (bitmap, 4 KiB pages)
- **Sv39 virtual memory** (3-level page tables, identity-mapped kernel, paging enabled via `satp`)
- **Kernel heap** (`kmalloc` / `kfree`) in its own virtual region with block splitting, coalescing and automatic page growth
- **Timer interrupts at 100 Hz**: M-mode catches the hardware timer and forwards each tick to the kernel (S-mode), which counts it. Time is read with `rdtime`
- Machine-mode trap handler that saves/restores all registers and prints `mcause` for unhandled traps
- **Supervisor-mode trap handler in C**: exceptions are delegated to S-mode, decoded by name and reported with `scause` / `sepc` / `stval`
- Standalone `timer/` test program for reading `mtime`
- Make-based build system with `size` and `pages` inspection targets

### Boot output

```text
[INFO] KnocOS starting
[INFO] Supervisor interrupts enabled
[INFO] Page memory initialized
[INFO] Virtual memory initialized
[INFO] Kernel page tables ready
[INFO] Kernel heap mapping prepared
[INFO] Enabling Sv39
[INFO] Sv39 enabled
[INFO] Kernel heap activated
[INFO] Allocation A successful
...
[INFO] Kernel heap 4.0 stress test passed
[INFO] Supervisor timer interrupts verified
[TRAP] Breakpoint
[TRAP] sepc   = 0x0000000080000790
[INFO] Supervisor trap handler verified
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

## 🚧 Currently Building: Interrupts & Traps (Phase 4)

Memory management (Phase 3) is complete. Work is now on **hardware interrupts and trap handling**.

Recent progress:

| Commit | Milestone |
|---|---|
| `9a094c3` | Integrate kernel timer interrupts |
| `42458f8` | Verify machine timer interrupts |
| `2a69e89` | Fix timer interrupt and reliable timer test, R-X / RW- linker segments |
| *(uncommitted)* | Timer cleanup: shared constants in `timer.h`, `timer_set_next()` removed |
| *(uncommitted)* | Supervisor-mode trap handler in C |
| *(uncommitted)* | Timer interrupts forwarded to Supervisor mode |

What works right now:

- `boot.S` arms the first timer deadline, enables `mie.MTIE`, and sets `mcounteren.TM` so S-mode can use `rdtime`
- `machine_trap` handles machine timer interrupts: it calls `timer_interrupt()`, which does `mtimecmp += TIMER_INTERVAL` (no drift) and forwards the tick by setting `mip.SSIP`
- `mideleg` delegates the supervisor software interrupt to S-mode, and `trap_enable_interrupts()` turns on `sie.SSIE` and `sstatus.SIE`
- The kernel's `supervisor_trap_handler()` receives the forwarded tick, clears `sip.SSIP` and calls `timer_tick()`
- `boot.S` delegates exceptions to Supervisor mode with `medeleg = 0xB1FF`, covering misaligned/access faults, illegal instruction, breakpoint, U-mode `ecall` and page faults
- `supervisor_trap` (in `boot.S`) saves 31 registers into a `trap_frame_t`, calls `supervisor_trap_handler()` in C, restores the registers and `sret`s
- `supervisor_trap_handler()` reads `scause`, `sepc` and `stval`. A breakpoint (`ebreak`) is logged and skipped (`sepc += 4`), and anything else is decoded by name, printed, and ends in `panic`
- Self-tests: the timer test waits for 5 kernel ticks (1 s timeout, and they must not arrive faster than 10 ms apart), and the trap test runs `ebreak` and checks the handler ran and returned

Next steps in this phase:

- [ ] PLIC (interrupt controller)
- [ ] UART driver with input interrupts
- [ ] Device abstraction

---

## Architecture

```text
Architecture: RISC-V 64-bit
ISA:          RV64G
ABI:          LP64D
Machine:      QEMU virt
Kernel base:  0x80000000
RAM:          128 MiB (0x80000000 – 0x88000000)
Page size:    4 KiB
Paging:       Sv39
```

### Privilege flow

```text
QEMU reset
   │
   ▼
_start (Machine mode)          boot/boot.S
   ├─ set stack pointer
   ├─ PMP: allow all memory
   ├─ mtvec = machine_trap
   ├─ stvec = supervisor_trap
   ├─ medeleg: exceptions → S-mode
   ├─ mideleg: supervisor software interrupt → S-mode
   ├─ mstatus.MPP = S, MPIE = 1
   ├─ mcounteren.TM = 1 (rdtime)
   ├─ mtimecmp = mtime + TIMER_INTERVAL
   ├─ mie.MTIE = 1
   └─ mret
   │
   ▼
kernel_main (Supervisor mode)  kernel/main.c
   ├─ trap_enable_interrupts()  sie.SSIE + sstatus.SIE
   ├─ page_init()     physical page allocator
   ├─ vm_init()       build Sv39 page tables
   ├─ heap_init()     map first heap page
   ├─ vm_enable()     write satp, sfence.vma
   ├─ heap_activate() create first heap block
   ├─ heap stress test
   ├─ timer interrupt test
   └─ supervisor trap test (ebreak)

Timer interrupt ──► machine_trap (M-mode) ──► timer_interrupt(): re-arm + set mip.SSIP ──► mret
                         └──► supervisor_trap (S-mode) ──► clear sip.SSIP, timer_tick() ──► sret
Exception ────────► supervisor_trap (S-mode) ──► supervisor_trap_handler() ──► sret / panic
```

### Physical memory layout

```text
0x02004000 ─────────────── CLINT mtimecmp
0x0200BFF8 ─────────────── CLINT mtime
0x10000000 ─────────────── UART0
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
0x88000000 ─────────────── RAM END
```

(Exact addresses of `kernel_end` / `stack_top` change as the kernel grows. Use `make size` or `nm` to see them.)

### Virtual memory layout (Sv39)

| Virtual range | Maps to | Flags | Purpose |
|---|---|---|---|
| `0x80000000 – 0x88000000` | same (identity) | `R W X` | Kernel + all RAM |
| `0x10000000` (1 page) | same (identity) | `R W` | UART |
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
│   ├── logging.c/h   # UART logging and panic()
│   ├── memory.c      # Early RAM / kernel / stack accounting helpers
│   ├── page.c/h      # Bitmap physical page allocator
│   ├── vm.c/h        # Sv39 page tables, mapping, satp enable, debug
│   ├── heap.c/h      # kmalloc / kfree kernel heap
│   ├── timer.c/h     # rdtime, tick counter, timer interrupt, shared timer constants
│   └── trap.c/h      # Supervisor trap frame and C trap handler
├── timer/
│   ├── timer.S       # Standalone program that prints mtime in a loop
│   └── linker.ld
├── Makefile
└── README.md
```

---

## Subsystems

### Boot: `boot/boot.S`

- Sets `sp` to `stack_top`
- Configures PMP entry 0 as NAPOT covering all memory with `RWX`, which Supervisor mode needs in order to run
- Installs `machine_trap` (`mtvec`) and `supervisor_trap` (`stvec`)
- Delegates exceptions to S-mode (`medeleg = 0xB1FF`) and the supervisor software interrupt (`mideleg = SIP_SSIP`)
- Sets `mepc = supervisor_start`, `MPP = Supervisor`, `MPIE = 1`
- Enables `rdtime` for S-mode (`mcounteren.TM`), arms the first `mtimecmp`, enables `MTIE` and executes `mret`
- Includes `kernel/timer.h` for `CLINT_MTIME`, `CLINT_MTIMECMP` and `TIMER_INTERVAL`
- `machine_trap` saves 31 registers on a 256-byte stack frame, dispatches timer interrupts to C and prints the `mcause` hex value for anything else
- `supervisor_trap` saves 31 registers as a `trap_frame_t`, calls `supervisor_trap_handler(frame)` and returns with `sret`

### Linker script: `boot/linker.ld`

- Kernel starts at `0x80000000`
- Program headers: `text` (`FLAGS(5)` = R-X) for `.text`/`.rodata`, `data` (`FLAGS(6)` = RW-) for `.data`/`.bss`
- Exports `kernel_start`, `kernel_end`, `stack_bottom`, `stack_top`
- 16 KiB stack, 16-byte aligned

### Logging: `kernel/logging.c`

- `log_info()` → `[INFO] ...`
- `log_warn()` → `[WARN] ...`
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
- `vm_init()` identity-maps all RAM (`RWX`) and the UART (`RW`)
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
- Breakpoints are logged, counted and skipped (`sepc += 4`)
- Every other trap prints `scause`, `sepc`, `stval`, `ra` and `sp`, then panics
- `trap_breakpoint_count()` is used by the self-test
- `trap_enable_interrupts()` enables supervisor interrupts (`sie.SSIE`, `sstatus.SIE`)
- `trap.h` holds `SIP_SSIP`, `SIE_SSIE` and `SSTATUS_SIE`, shared with `boot.S`

### Early memory info: `kernel/memory.c`

Helpers from the first milestone: total RAM (B/KB/MB/GB), kernel size, stack size, used/free bytes. These are not called by `kernel_main` right now; the page allocator does the real accounting.

---

## Building

Requirements: `riscv64-unknown-elf-gcc`, `riscv64-unknown-elf-ld`, `qemu-system-riscv64`, `python3` (for inspection targets).

```bash
make            # build knocos.elf
make run        # boot KnocOS in QEMU (exit: Ctrl-A then X)
make clean      # remove build artifacts
make size       # kernel / stack size info
make pages      # physical page layout summary
make timer-test # run the standalone mtime printer (timer.elf)
```

> **Note:** The Makefile does not list `boot/linker.ld` or every header as a dependency. After editing those, run `make clean && make`.

Compiler flags:

```text
-march=rv64g -mabi=lp64d -mcmodel=medany
-ffreestanding -fno-pie -fno-pic -nostdlib -nostartfiles -nodefaultlibs
```

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
- [x] Separate R-X / RW- ELF segments *(uncommitted)*

### Phase 4: Hardware & Interrupts 🚧 *(in progress)*

- [x] Timer (CLINT `mtime` / `mtimecmp`)
- [x] Machine-mode trap entry and register save/restore
- [x] Timer interrupt handling and tick counter
- [x] Reliable timer self-test
- [x] Supervisor-mode trap handling in C
- [x] Exception decoding (page faults, illegal instruction, ...)
- [x] Timer interrupts forwarded to Supervisor mode
- [ ] Interrupt controller (PLIC)
- [ ] UART driver with input
- [ ] Device abstraction

### Phase 5: Processes & Scheduling

- [ ] Process structure
- [ ] Context switching
- [ ] Scheduler (timer-driven preemption)
- [ ] User mode
- [ ] System calls
- [ ] Multiple processes

### Phase 6: Storage & Filesystems

- [ ] Block device (virtio)
- [ ] Disk driver
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

## Philosophy

KnocOS is being built from the lowest practical level upward:

```text
Hardware → RISC-V ISA → Boot code → Linker → Kernel → Memory management
→ Interrupts → Processes → System calls → Filesystem → User space → Applications
```

The goal is not simply to produce an operating system. The goal is to understand **why every layer exists and how the layers communicate with each other.**

---

## Status

**Early development: experimental / educational.**

Boot, logging, physical memory, Sv39 paging and the kernel heap are working. Timer interrupts (forwarded to the kernel) and Supervisor-mode exception handling are working. The interrupt controller and device drivers are next.
