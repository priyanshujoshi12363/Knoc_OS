
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
- Kernel logging (`log_info`, `log_warn`) and a `panic` handler
- **Physical page allocator** (bitmap, 4 KiB pages)
- **Sv39 virtual memory** (3-level page tables, identity-mapped kernel, paging enabled via `satp`)
- **Kernel heap** (`kmalloc` / `kfree`) in its own virtual region with block splitting, coalescing and automatic page growth
- **Machine timer interrupts** via the CLINT (`mtime` / `mtimecmp`) with a tick counter
- Machine-mode trap handler that saves/restores all registers and prints `mcause` for unhandled traps
- Standalone `timer/` test program for reading `mtime`
- Make-based build system with `size` and `pages` inspection targets

### Boot output

```text
[INFO] KnocOS starting
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
[INFO] Timer interrupt and tick counter verified
```

---

## 🚧 Currently Building: Interrupts & Traps (Phase 4)

Memory management (Phase 3) is complete. Work is now on **hardware interrupts and trap handling**.

Recent progress (from the git history):

| Commit | Milestone |
|---|---|
| `9a094c3` | Integrate kernel timer interrupts |
| `42458f8` | Verify machine timer interrupts |
| *(uncommitted)* | `boot/linker.ld`: add `PHDRS` so text is `R-X` and data is `RW-`, which removes the linker warning *"LOAD segment with RWX permissions"* |

What works right now:

- `boot.S` enables the machine timer interrupt (`mie.MTIE`) and installs `machine_trap` in `mtvec`
- `machine_trap` saves all registers, checks `mcause == 0x8000000000000007` (machine timer interrupt), calls `timer_interrupt()` in C, restores registers and `mret`s
- `timer_interrupt()` increments a tick counter and re-arms `mtimecmp` every `100000` ticks (≈10 ms at QEMU's 10 MHz timebase)
- Any other machine trap prints `[TRAP] mcause=0x...` and halts

Known issue:

- **The timer self-test in `kernel/main.c` can fail** with `[PANIC] Timer interrupt test failed`. The first deadline is set to `now + 1000000` (≈100 ms), but the busy-wait loop (`10000000` iterations) often finishes sooner under QEMU, so the tick counter is still `0` when checked. Interrupts themselves work: with a longer wait (or a shorter first deadline) the test passes.

Next steps in this phase:

- [ ] Fix the timer self-test timing
- [ ] Supervisor-mode trap handler in C (`stvec` currently only prints a message and halts)
- [ ] Delegate / forward timer interrupts to Supervisor mode (`mideleg`, `medeleg`, `sip.STIP`)
- [ ] Decode and report exceptions (page faults, illegal instructions) with `sepc` / `stval`
- [ ] PLIC (interrupt controller) and UART input interrupts

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
   ├─ mstatus.MPP = S, MPIE = 1
   ├─ mie.MTIE = 1
   └─ mret
   │
   ▼
kernel_main (Supervisor mode)  kernel/main.c
   ├─ arm first timer interrupt
   ├─ page_init()     physical page allocator
   ├─ vm_init()       build Sv39 page tables
   ├─ heap_init()     map first heap page
   ├─ vm_enable()     write satp, sfence.vma
   ├─ heap_activate() create first heap block
   ├─ heap stress test
   └─ timer interrupt test

Timer interrupt ──► machine_trap (M-mode) ──► timer_interrupt() ──► mret
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
│   ├── boot.S        # M-mode entry, PMP, trap vectors, mret to S-mode, trap handler
│   └── linker.ld     # Kernel layout, segments, stack
├── kernel/
│   ├── main.c        # kernel_main: init sequence + self-tests
│   ├── logging.c/h   # UART logging and panic()
│   ├── memory.c      # Early RAM / kernel / stack accounting helpers
│   ├── page.c/h      # Bitmap physical page allocator
│   ├── vm.c/h        # Sv39 page tables, mapping, satp enable, debug
│   ├── heap.c/h      # kmalloc / kfree kernel heap
│   └── timer.c/h     # CLINT timer read/set, tick counter, interrupt handler
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
- Sets `mepc = supervisor_start`, `MPP = Supervisor`, `MPIE = 1`, enables `MTIE` and executes `mret`
- `machine_trap` saves 31 registers on a 256-byte stack frame, dispatches timer interrupts to C and prints the `mcause` hex value for anything else

### Linker script: `boot/linker.ld`

- Kernel starts at `0x80000000`
- Program headers: `text` (`FLAGS(5)` = R-X) for `.text`/`.rodata`, `data` (`FLAGS(6)` = RW-) for `.data`/`.bss`
- Exports `kernel_start`, `kernel_end`, `stack_bottom`, `stack_top`
- 16 KiB stack, 16-byte aligned

### Logging: `kernel/logging.c`

- `log_info()` → `[INFO] ...`
- `log_warn()` → `[WARN] ...`
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

- `timer_read()` reads CLINT `mtime` (`0x0200BFF8`)
- `timer_set_next()` writes `mtimecmp` (`0x02004000`)
- `timer_interrupt()` increments `ticks` and re-arms for `+100000`
- `timer_ticks()` returns the tick count

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

> **Note:** The Makefile does not list `boot/linker.ld` or most headers as dependencies. After editing those, run `make clean && make`.

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
- [ ] Reliable timer self-test
- [ ] Supervisor-mode trap handling in C
- [ ] Exception decoding (page faults, illegal instruction, ...)
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

Boot, logging, physical memory, Sv39 paging and the kernel heap are working. Timer interrupts are working, and the rest of interrupt/trap handling is under active development.
