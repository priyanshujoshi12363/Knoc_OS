# KnocOS Notes: From Basics to Advanced

These notes explain **how KnocOS works and why it is built this way**, starting from zero. Read them in order the first time. Later, use them as a reference.

Each part links theory to the actual KnocOS code, so you can open the file and see the idea in practice.

> If you understand these notes, you understand every line of KnocOS.

---

## Contents

- **Part 1: Basics:** what an OS is, bare metal, QEMU, RISC-V
- **Part 2: Building and booting:** toolchain, linker script, `boot.S`, privilege modes
- **Part 3: Talking to hardware:** memory-mapped I/O, UART, logging
- **Part 4: Memory:** the device tree, the buddy allocator, virtual memory (Sv39) and megapages, kernel heap, spinlocks
- **Part 5: Traps, interrupts, devices and processes:** exceptions, timer, PLIC, keyboard input, the device driver model, the disk, processes and the AI-aware scheduler, the AI space, fault containment and the warm kernel restart, user mode and system calls, wait queues, the KnocFS filesystem, the shell
- **Part 6: Engineering:** Makefile, tests, CI, versioning
- **Part 7: Debugging:** tools, reading a crash, bugs we hit and fixed
- **Part 8: What comes next:** NN runtime, AI-OS
- **Cheat sheets:** addresses, CSRs, commands, glossary

---

# Part 1: Basics

## 1.1 What is an operating system?

An OS is the program that **manages the hardware** and **gives other programs a safe, simple way to use it**.

| Job | Windows example | KnocOS today |
|---|---|---|
| Start the computer | Windows boot logo | `boot.S` → `kernel_main` |
| Manage memory | Gives Chrome its own RAM | Page allocator, Sv39 paging, heap |
| React to hardware | Key press → Notepad | UART interrupt → echo |
| Share the CPU | Chrome + Spotify at once | Processes + AI-aware scheduler |
| Store files | C: drive, NTFS | Disk driver ✅, filesystem not yet |
| Protect programs from each other | One app can't read another's memory | Not yet (needs user mode) |

The **kernel** is the core of the OS that runs with full hardware power. On Windows it's `ntoskrnl.exe`, on Linux it's `vmlinuz`, and in KnocOS it's `knocos.elf`.

## 1.2 Bare metal

Normal programs run **on top of** an OS: `printf` asks the OS to print, and `malloc` asks the OS for memory.

KnocOS runs on **bare metal**: nothing is underneath it. There's no `printf`, no `malloc`, no files. That's why:

- We compile with `-ffreestanding -nostdlib`, meaning no C standard library.
- We wrote our own printing (`uart.c`), our own `kmalloc` (`heap.c`), and so on.
- The only header we use from the compiler is `<stdint.h>` (for `uint64_t` and similar), because it contains only type definitions and no code.

## 1.3 QEMU: a computer inside your computer

**QEMU** emulates a whole RISC-V computer in software. KnocOS thinks it's running on real hardware.

```bash
qemu-system-riscv64 -machine virt -bios none -nographic -kernel knocos.elf
```

| Flag | Meaning |
|---|---|
| `-machine virt` | Emulate QEMU's generic RISC-V board, called "virt" |
| `-bios none` | No firmware, so KnocOS starts first, in the most powerful mode |
| `-nographic` | No window: the serial port (UART) is connected to your terminal |
| `-kernel knocos.elf` | Load our kernel into RAM at `0x80000000` and start it |

**Windows comparison:** like VirtualBox running a VM, except here we wrote the OS inside it.

## 1.4 RISC-V in 5 minutes

RISC-V is a CPU **instruction set**, the "language" the CPU understands, like x86 in Intel/AMD PCs or ARM in phones. It's open and simple, which makes it good for learning.

**RV64G** means 64-bit registers plus the standard extensions (multiply, atomics, floating point).

### Registers

32 general registers, each 64 bits. Everyone uses their ABI names:

| Name | Use |
|---|---|
| `zero` | Always 0 |
| `ra` | Return address (where a function returns to) |
| `sp` | Stack pointer |
| `gp`, `tp` | Global pointer, thread pointer |
| `a0`–`a7` | Function arguments and return values |
| `t0`–`t6` | Temporaries (a function may overwrite them) |
| `s0`–`s11` | Saved registers (a function must preserve them) |

This matters for traps: a trap handler must **save every register** it might change, because the interrupted code expects them unchanged.

### CSRs: the CPU's control panel

**Control and Status Registers** configure the CPU itself: which mode it's in, where to jump on a trap, which interrupts are on. They're read and written with special instructions:

```asm
csrr t0, scause      # read CSR into t0
csrw stvec, t0       # write t0 into CSR
csrs mie, t0         # set bits (OR)
csrc mstatus, t0     # clear bits (AND NOT)
```

The CSRs KnocOS uses are listed in the [cheat sheet](#csr-cheat-sheet).

---

# Part 2: Building and Booting

## 2.1 From C code to a running kernel

```text
kernel/main.c ──gcc──► kernel/main.o ─┐
kernel/heap.c ──gcc──► kernel/heap.o ─┤
boot/boot.S   ──gcc──► boot/boot.o   ─┼──ld + linker.ld──► knocos.elf ──QEMU──► running
...                                   ┘
```

- **Compiler** (`riscv64-unknown-elf-gcc`): turns each `.c`/`.S` file into an **object file** (`.o`) of machine code.
- **Linker** (`riscv64-unknown-elf-ld`): joins all `.o` files into one **ELF** file and decides **where in memory** everything goes.
- **Cross-compiler:** your PC is x86-64, but the output is RISC-V code. "unknown-elf" means "for bare metal, no OS".

`.S` (capital S) files are assembly that goes through the C preprocessor first, so `boot.S` can `#include "../kernel/timer.h"` and use `#define` constants.

## 2.2 The linker script (`boot/linker.ld`)

It tells the linker how to lay out memory:

```text
0x80000000  kernel_start
            .text    machine code          ┐ segment "text"  R-X (read + execute)
            .rodata  constant data, strings┘
            .data    initialized globals   ┐ segment "data"  RW- (read + write)
            .bss     zeroed globals        ┘
            kernel_end
            (align 16)
            stack_bottom
            16 KiB stack
            stack_top
```

- `0x80000000` is where RAM starts on QEMU `virt`, and where QEMU jumps after reset.
- Symbols like `kernel_end` and `stack_top` have no storage. They are **addresses**, which C code reads as `extern char stack_top;` and `(uintptr_t)&stack_top`.
- **Segments with permissions:** code is read + execute, data is read + write. Nothing is writable *and* executable, which is a basic security rule (it's why the "RWX segment" linker warning existed and was fixed).

## 2.3 Privilege modes

RISC-V has three privilege levels:

| Mode | Power | Windows equivalent | KnocOS |
|---|---|---|---|
| **M**achine | Everything, including raw hardware | BIOS / UEFI firmware | `boot.S` startup, `machine_trap` |
| **S**upervisor | Kernel: page tables, traps, devices | `ntoskrnl.exe` | `kernel_main` and everything in `kernel/` |
| **U**ser | Apps: restricted | Chrome, Notepad | Not yet |

Why not just run the kernel in M-mode? Because **S-mode is where virtual memory and the normal kernel tools live**, and it's how real systems work (Linux runs in S-mode with OpenSBI firmware in M-mode). KnocOS plays both roles: a tiny "firmware" in M-mode and the kernel in S-mode.

## 2.4 The boot sequence (`boot/boot.S`)

QEMU starts the CPU in **M-mode** at `_start` (`0x80000000`). Step by step:

| Step | Code | Why |
|---|---|---|
| 0 | `csrr mhartid` | Both cores start here. **Core 1 → the AI space** (section 5.12). Core 0 continues |
| 1 | clear `.bss`, `la sp, stack_top` | Zero the uninitialized globals (a warm reboot doesn't), then give C code a stack |
| 2 | `pmpaddr0/1`, `pmpcfg0` | **PMP** (physical memory protection) blocks S-mode from all memory by default. Entry 0 **denies** the AI space region, entry 1 allows everything else |
| 2b | `satp = 0`, clear `sie`/`mie`/`mip` | After a warm reboot the CPU may still have paging on and old interrupt bits set. Start clean |
| 3 | `mscratch = machine_stack_top`, `mtvec = machine_trap` | M-mode gets its own private stack, and the address where the CPU jumps on an M-mode trap |
| 4 | `stvec = supervisor_trap` | Where the CPU jumps on an S-mode trap |
| 5 | `medeleg = 0xB1FF` | **Delegate exceptions** (page faults, illegal instruction, ...) to S-mode so the kernel handles them |
| 6 | `mideleg = SSIP \| SEIP` | Delegate the software interrupt (forwarded timer) and external interrupts (devices) to S-mode |
| 7 | `mepc = supervisor_start` | Where `mret` will "return" to |
| 8 | `mstatus.MPP = S`, `MPIE = 1` | `mret` will switch to **S-mode** with interrupts enabled |
| 9 | `mcounteren.TM = 1` | Allow S-mode to use the `rdtime` instruction |
| 10 | `mtimecmp = mtime + TIMER_INTERVAL` | Set the first timer alarm |
| 11 | `mie.MTIE = 1` | Enable the machine timer interrupt |
| 12 | `mret` | **Drop from M-mode to S-mode** and jump to `supervisor_start` → `kernel_main` |

**Key idea:** `mret` is normally "return from trap", but it's also the standard way to **enter a lower privilege mode**. We set up `mepc` and `MPP` as if we were returning from a trap that came from S-mode.

## 2.5 The kernel init sequence (`kernel/main.c`)

```text
trap_enable_interrupts()   kernel accepts interrupts
page_init()                physical page allocator
vm_init()                  build page tables
heap_init()                map the first heap page
vm_enable()                turn on Sv39 paging
heap_activate()            create the first heap block
self-tests                 heap, timer, trap
plic_init()                interrupt controller
uart_register(), ...       fill the device table
device_init_all()          start every driver, enable its IRQ
echo loop                  interactive, Ctrl-D powers off
```

The order matters. For example, the heap lives at virtual address `0x2000000000`, which only exists **after** paging is on, so `heap_activate()` must come after `vm_enable()`.

---

# Part 3: Talking to Hardware

## 3.1 Memory-mapped I/O (MMIO)

On RISC-V, devices appear as **addresses**. Reading or writing those addresses talks to the device instead of RAM.

```c
volatile uint8_t *uart = (volatile uint8_t *)0x10000000;
*uart = 'A';   // sends 'A' out of the serial port
```

**`volatile`** tells the compiler "every read/write here matters, don't optimize it away or reorder it". Without it, the compiler might skip writes it thinks are useless. **Every hardware pointer must be `volatile`.**

The same rule applies to variables changed by interrupts (`ticks`, the UART ring buffer indexes): the compiler can't see the interrupt, so they must be `volatile` too.

## 3.2 The UART (`kernel/uart.c`)

The UART is an **NS16550** serial chip (the same design as the COM1 port on old PCs). The registers we use:

| Offset | Name | Use |
|---|---|---|
| 0 | RBR / THR | Read a received byte / write a byte to send |
| 1 | IER | Interrupt enable: bit 0 = "interrupt me when a byte arrives" |
| 2 | FCR | FIFO control: enable and clear the FIFOs |
| 3 | LCR | Line control: `0x03` = 8 data bits, no parity, 1 stop bit |
| 5 | LSR | Line status: bit 0 = data ready, bit 5 = transmitter empty |

- `uart_putc()` waits until LSR bit 5 says "ready to send", then writes THR.
- The driver's interrupt handler reads RBR while LSR bit 0 says "data ready".

## 3.3 Logging (`kernel/logging.c`)

Built on the UART: `log_info()` prints `[INFO] ...`, `log_warn()` prints `[WARN]`, `log_trap()` prints `[TRAP]`, and `panic()` prints `[PANIC] ...` and **halts forever**. Panic is for "this should never happen, stop before making it worse".

---

# Part 4: Memory

## 4.1 Physical memory and pages

RAM starts at `0x80000000`. **How much** there is depends on the machine: `make run` gives QEMU 2 GiB (`-m 2G`), so RAM ends at `0x100000000`. Up to v0.9.0 KnocOS hard-coded 128 MiB. Now it asks the firmware (4.2).

Memory is managed in **pages** of **4 KiB (4096 bytes)**:

```text
2 GiB / 4 KiB = 524,288 pages
```

**Why pages?** Tracking every byte is impossible. Tracking pages is easy, and the CPU's paging hardware works in 4 KiB pages too.

## 4.2 The device tree: asking how much RAM there is (`kernel/fdt.c`)

A real computer can have any amount of RAM, so the kernel must **ask**. On RISC-V and ARM, firmware answers with a **device tree**: a binary file describing the machine (RAM, CPUs, devices and their addresses and IRQs). QEMU builds one and passes its address in register **`a1`** to every core at power-on. **Windows/PC comparison:** ACPI tables and the UEFI memory map do the same job on a PC.

The flattened format is a header plus a list of big-endian tokens:

```text
BEGIN_NODE ""                 the root
  PROP #address-cells = 2     addresses are 2 × 32 bits
  PROP #size-cells    = 2
  BEGIN_NODE "memory@80000000"
    PROP reg = <0x0 0x80000000  0x0 0x80000000>    start, size
  END_NODE
  BEGIN_NODE "cpus"
    BEGIN_NODE "cpu@0" ... END_NODE
    BEGIN_NODE "cpu@1" ... END_NODE
  END_NODE
END_NODE
END
```

`fdt_parse()` walks the tokens, reads `/memory` → `reg` and counts `/cpus/cpu@N`:

```text
[INFO] Device tree at 0x00000000BFE00000: RAM 2048 MiB at 0x0000000080000000, 2 CPUs
```

Three details:
- **The tree lives in RAM** (QEMU puts it just below 3 GiB, at `0xBFE00000`), so the page allocator must reserve it, or it would be handed out and overwritten
- **Why at least 1 GiB?** With 512 MiB, QEMU puts the tree at `0x9FE00000`, inside the AI space, where PMP stops the kernel from reading it. The kernel checks this and stops with a clear message
- **Warm restart:** core 0 jumps to `_start` again, but `a1` then holds garbage. The AI space remembered the address from its own power-on `a1` and puts it back before releasing core 0

## 4.3 The buddy page allocator (`kernel/page.c`)

Up to v0.9.0, KnocOS used a **bitmap** (1 bit per page) with a first-fit search. That can't answer "give me **64 MiB in one piece**", which AI model weights need. A **buddy allocator** can. Linux uses one for the same reason.

**The idea:** free memory is kept as blocks of **2^order pages**, each aligned to its own size:

| Order | Block size | |
|---|---|---|
| 0 | 4 KiB | one page |
| 9 | 2 MiB | one megapage |
| 14 | 64 MiB | a small model |
| 18 | 1 GiB | the biggest block |

There's one free list per order. **Allocating** takes the smallest free block that fits and cuts it in half until it's the right size. Each unused half goes on the free list for its size:

```text
want 4 KiB, only a 16 KiB block is free:
[ 16 KiB                    ]  split
[ 8 KiB      ][ 8 KiB free  ]  split
[4K][4K free ][ 8 KiB free  ]  → return the first 4K
```

**Freeing** is the clever part. Every block has exactly one **buddy**, the other half it was split from, and its index is one XOR away: `buddy = index ^ 2^order`. If the buddy is free too, they merge into one block of the next order, and this repeats. So memory **heals itself** back into big blocks. The self-test proves it: after 1024 single pages are freed in a scrambled order, the largest free block is back to 1 GiB.

**Bookkeeping:** one byte per page (`0x80` = head of a free block, `0x40` = head of an allocated block, low 5 bits = order). For 2 GiB that's 512 KiB, placed right after the boot stack. The free lists themselves cost nothing: each free block stores its `next`/`prev` pointers **inside its own first page** (it's free, so nobody else is using it).

**At boot**, `page_init()` subtracts the reserved areas (kernel + stack + page info, the AI space, the device tree) and cuts every free range into the largest aligned blocks that fit. With 2 GiB: 1791 MiB free, largest block 1 GiB (`0xC0000000–0xFFFFFFFF`).

**Windows comparison:** Windows' memory manager keeps a database of every physical page (the "PFN database"), and large-page allocations need contiguous physical memory just like this.

## 4.4 Why virtual memory?

Without virtual memory, every program sees real physical addresses:
- Programs could read and overwrite each other's memory.
- Every program would need to be placed at a different address.
- Memory can't be "faked" (for example a big heap made of scattered pages).

With **virtual memory**, each address a program uses goes through a **translation table** (page table) set up by the kernel:

```text
virtual address  ──► page tables ──►  physical address
0x2000000000     ──►               ──► 0x80009000 (whatever page_alloc gave)
```

**Windows comparison:** every Windows process has its own address space. Chrome's `0x10000` and Spotify's `0x10000` are different physical memory.

## 4.5 Sv39: RISC-V's paging scheme (`kernel/vm.c`)

**Sv39** = 39-bit virtual addresses, 3 levels of page tables.

A virtual address is split like this:

```text
 38        30 29        21 20        12 11          0
┌────────────┬────────────┬────────────┬─────────────┐
│  VPN[2]    │  VPN[1]    │  VPN[0]    │   offset    │
│  9 bits    │  9 bits    │  9 bits    │   12 bits   │
└────────────┴────────────┴────────────┴─────────────┘
```

- Each page table has **512 entries** (2^9) of 8 bytes = exactly **one 4 KiB page**.
- The **offset** (12 bits = 4096) is the byte inside the page.

### Worked example: translating `0x2000000000` (the heap)

```text
VPN[2] = (0x2000000000 >> 30) & 0x1FF = 128
VPN[1] = (0x2000000000 >> 21) & 0x1FF = 0
VPN[0] = (0x2000000000 >> 12) & 0x1FF = 0
offset =  0x2000000000 & 0xFFF        = 0

root table[128]  → points to a level-1 table
level-1 table[0]   → points to a level-0 table
level-0 table[0]   → points to the physical page (for example 0x80009000)
physical address = 0x80009000 + offset 0
```

`vm_debug(address)` prints this whole walk, which is a great way to learn it.

### Page table entry (PTE)

```text
 63      54 53                  10 9  8 7 6 5 4 3 2 1 0
┌──────────┬──────────────────────┬────┬─┬─┬─┬─┬─┬─┬─┬─┐
│ reserved │  PPN (physical page) │RSW │D│A│G│U│X│W│R│V│
└──────────┴──────────────────────┴────┴─┴─┴─┴─┴─┴─┴─┴─┘
```

| Bit | Meaning |
|---|---|
| V | Valid: the entry is in use |
| R / W / X | Read / write / execute allowed |
| U | User mode may access |
| PPN | Physical page number (physical address >> 12) |

If R, W and X are all 0 (and V=1), the entry points to the **next level table**. Otherwise it's a **leaf** that maps a page. `vm_make_pte()` builds entries and `vm_map()` walks and creates tables on demand.

### Turning paging on

```c
satp = (8 << 60) | (root_table_address >> 12);   // mode 8 = Sv39
sfence.vma                                       // flush the translation cache (TLB)
```

**TLB** (translation lookaside buffer): the CPU caches translations. After changing page tables, `sfence.vma` tells it to forget old ones.

### Identity mapping

KnocOS maps RAM with **virtual = physical** (`0x80000000 → 0x80000000`). That way, the code keeps running at the same addresses the moment paging turns on. Devices are identity-mapped too.

### Megapages: 2 MiB in one entry

A leaf entry doesn't have to be at level 0. If the **level-1** entry has R/W/X set, it maps a whole **2 MiB** region directly (a "megapage"), and there's no level-0 table below it. The address offset is then 21 bits instead of 12. (A level-2 leaf would be a 1 GiB "gigapage".)

| Mapping 2 GiB of RAM with | Entries | Page table memory |
|---|---|---|
| 4 KiB pages | 524,288 | 1024 level-0 tables = **4 MiB** |
| 2 MiB megapages | 1024 | 2 level-1 tables = **8 KiB** |

It's also faster: one TLB entry covers 2 MiB instead of 4 KiB. That matters for AI, because a model's weights are read from start to end over and over. `vm_map_range()` uses a megapage whenever both addresses are 2 MiB aligned and at least 2 MiB is left, and 4 KiB pages otherwise. With 2 GiB, RAM takes 1024 megapages and the PLIC 2 more (`RAM mapped with 2 MiB megapages: 1026`).

**Why the heap moved:** the heap used to live at virtual `0x90000000`, which was fine while RAM ended at `0x88000000`. With 2 GiB, `0x90000000` is real RAM, identity-mapped by a megapage, so the heap moved far above any RAM, to `0x2000000000` (128 GiB).

**Critical lesson:** after paging is on, **any address not in the page tables causes a page fault**, including devices. That's why `vm_init()` maps the UART, the PLIC and the power device. M-mode ignores page tables, which is why the timer code running in M-mode doesn't need a mapping.

## 4.6 The kernel heap (`kernel/heap.c`)

Pages are 4 KiB, but the kernel often needs 24 bytes or 3000 bytes. The **heap** splits pages into variable-size blocks: that's `kmalloc(size)` / `kfree(ptr)`.

```text
0x2000000000
┌────────┬───────────┬────────┬──────────────┬────────┬─────────
│ header │  3000 B   │ header │    free      │ header │ ...
│ size   │ (in use)  │ size   │              │        │
│ free=0 │           │ free=1 │              │        │
│ next ──┼──────────►│ next ──┼─────────────►│        │
└────────┴───────────┴────────┴──────────────┴────────┴─────────
```

| Technique | What it does | Why |
|---|---|---|
| **First fit** | Use the first free block that's big enough | Simple and fast enough |
| **Splitting** | A big free block is cut to the requested size, and the rest stays free | Don't waste a whole block on a small request |
| **Coalescing** | On `kfree`, neighbouring free blocks merge into one | Avoid "fragmentation": many small free pieces that can't fit big requests |
| **Growth** | If nothing fits, `page_alloc()` a new page and map it at the heap end | The heap grows as needed, up to 256 MiB |
| **Alignment** | Sizes are rounded up to 8 bytes | `uint64_t` values must be 8-byte aligned |
| **Safety** | `kfree` ignores NULL, unknown pointers and double frees | Bugs shouldn't corrupt the heap |

**Windows comparison:** the Windows kernel's "pool allocator" (`ExAllocatePool`) does this job.

## 4.7 Spinlocks (`kernel/spinlock.c`)

Two things can go wrong when code shares data:
- **Preemption:** a timer tick switches to another process in the middle of `kmalloc`, and that process calls `kmalloc` too, on a half-updated block list
- **Two cores:** core 0 and core 1 touch the same memory at the same moment

A **spinlock** is a word that is 0 (free) or 1 (taken). Taking it has to be **atomic**, meaning no other core can sneak in between "read" and "write". RISC-V's `amoswap` swaps a value in memory in one step:

```c
while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE) != 0) { }   // amoswap.w.aq
...critical section...
__atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
```

`ACQUIRE`/`RELEASE` are **memory ordering**: no read or write from inside the critical section may move outside it. `spin_lock()` also **turns interrupts off** first. Otherwise a timer tick could switch to a process that then spins forever on a lock the sleeping process holds. The kernel heap and the page allocator both use one now.

### Sharing the UART between two cores

The AI space can't call kernel code, so it can't use `spinlock_t`. Instead both sides use one word in the mailbox, `console_owner` (0 free, 1 kernel, 2 AI space), taken with **compare-and-swap**: "if it's 0, make it mine". A core owns the UART for **a whole line** and gives it back at `\n`. Before v0.10.0 the two cores could mix characters on one line.

One twist: the owner might **crash in the middle of a line** (the kernel panics while printing). So waiting has a **10 ms timeout**, after which the line is printed anyway. A lock shared with code that can crash must never wait forever. `make test` checks that no line ever contains both kernel and AI space output.

---

# Part 5: Traps, Interrupts, Devices and Processes

## 5.1 Vocabulary

| Term | Meaning | Example |
|---|---|---|
| **Trap** | Anything that makes the CPU stop and jump to a handler | Both of the rows below |
| **Exception** | Caused **by the current instruction** (synchronous) | Page fault, illegal instruction, `ebreak`, `ecall` |
| **Interrupt** | Caused **by something outside** (asynchronous) | Timer alarm, key press |

## 5.2 What the CPU does on a trap (S-mode)

Automatically, in hardware:

1. `sepc` = address of the interrupted instruction
2. `scause` = why (top bit 1 = interrupt, 0 = exception; the rest = code)
3. `stval` = extra info (for example the bad address in a page fault)
4. `sstatus.SPIE = SIE`, then `SIE = 0` (interrupts off during the handler)
5. `sstatus.SPP` = previous mode
6. Jump to `stvec`

`sret` undoes this: it restores the interrupt setting and jumps to `sepc`. M-mode has the same set of registers with `m` names (`mepc`, `mcause`, `mret`, ...).

## 5.3 The trap handler (`boot.S` + `kernel/trap.c`)

```text
supervisor_trap (assembly):
  make room on the stack (256 bytes)
  save 31 registers        ← the interrupted code must not notice anything
  call supervisor_trap_handler(frame)   (C)
  restore 31 registers
  sret

supervisor_trap_handler (C):
  read scause / sepc / stval
  interrupt 1 (software) → forwarded timer tick → timer_tick()
  interrupt 9 (external) → ask the PLIC which device → run its driver
  exception 3 (breakpoint) → log, sepc += 4 (skip the ebreak), return
  anything else → print everything and panic
```

`trap_frame_t` in `trap.h` is a C struct with the **same layout** as the registers saved in assembly, so C can read them (`frame->ra`, `frame->sp`).

**Why `sepc += 4`?** After a trap, `sret` returns to `sepc`, which is the instruction that trapped. For `ebreak` that would trap again forever, so we skip it. Every instruction is 4 bytes (RV64G, no compressed instructions).

## 5.4 Delegation: who handles what

By default **all traps go to M-mode**. `medeleg` / `mideleg` send chosen ones straight to S-mode:

| Trap | Goes to | Why |
|---|---|---|
| Machine timer interrupt | M-mode | The hardware timer belongs to M-mode on RISC-V, and it can't be delegated |
| Forwarded tick (software interrupt) | S-mode | Delegated with `mideleg` |
| Device interrupts (external) | S-mode | Delegated with `mideleg` |
| Exceptions (page fault, ...) | S-mode | Delegated with `medeleg = 0xB1FF` |

## 5.5 The timer (`kernel/timer.c`)

Hardware (**CLINT**):
- `mtime` (`0x0200BFF8`): a counter that increases **10,000,000 times per second** (10 MHz)
- `mtimecmp` (`0x02004000`): when `mtime >= mtimecmp`, a machine timer interrupt fires

Tick rate: `TIMER_TICK_HZ = 100` gives an interval of `10,000,000 / 100 = 100,000` counts = **10 ms**.

```text
every 10 ms:
  M-mode machine_trap
    ├─ mtimecmp += 100000        re-arm (adding to the old deadline = no drift)
    └─ set mip.SSIP              "kernel, wake up"
  mret
  → S-mode supervisor_trap (software interrupt)
    ├─ clear sip.SSIP
    └─ timer_tick(): ticks++     (later: run the scheduler)
  sret
```

**Why SSIP and not STIP?** The kernel isn't allowed to clear STIP itself, but it can clear SSIP. xv6 uses the same trick. The newer **Sstc** extension lets S-mode program its own timer directly (`stimecmp`), which future versions could use.

**Reading the time:** `timer_read()` uses the `rdtime` instruction (allowed by `mcounteren.TM`). It works in S-mode and needs no page mapping.

**Why this matters:** this "heartbeat" lets the kernel take back the CPU every 10 ms. It's the foundation of multitasking.

## 5.6 The PLIC (`kernel/plic.c`)

The **Platform-Level Interrupt Controller** collects interrupts from all devices and delivers them to CPU cores.

```text
UART ──IRQ 10──┐
disk ──IRQ 1 ──┼──► PLIC ──► "external interrupt" ──► hart 0, S-mode (context 1)
rtc  ──IRQ 11──┘
```

| Register (for hart 0, S-mode) | Address | Use |
|---|---|---|
| Priority of IRQ n | `0x0C000000 + 4*n` | 0 = disabled, higher = more important |
| Enable bits | `0x0C002080` | Bit n = IRQ n enabled for this context |
| Threshold | `0x0C201000` | Only IRQs with priority > threshold are delivered |
| Claim / complete | `0x0C201004` | **Read** = "which IRQ?" (claim), **write** the number back = "done" (complete) |

**Claim/complete** is the key protocol: while an IRQ is claimed, the PLIC won't deliver it again. After completing, it can fire again.

## 5.7 Keyboard input: the full path

```text
You press "a" in the terminal
 → QEMU puts 'a' in the UART's receive FIFO
 → UART raises IRQ 10 (because IER bit 0 is on)
 → PLIC → external interrupt → CPU jumps to stvec (S-mode)
 → supervisor_trap saves registers → supervisor_trap_handler
 → plic_claim() returns 10
 → device_handle_irq(10): the device table says IRQ 10 belongs to uart0
 → uart0's interrupt handler: read every byte into the ring buffer
 → plic_complete(10)
 → sret: back to the echo loop
 → device_read(uart0) finds 'a' in the buffer → device_write(uart0, "a")
```

### The ring buffer

A fixed array used as a circle, with two indexes:

```text
 rx_tail (read here)        rx_head (write here)
     ▼                          ▼
[ . . h e l l o . . . . . . . . . ]   size 128
```

- **Interrupt** (producer) writes at `rx_head` and advances it.
- **Main loop** (consumer) reads at `rx_tail` and advances it.
- `head == tail` means empty, and `head + 1 == tail` means full (new bytes are dropped).
- One producer and one consumer on one core means no lock is needed. Just `volatile`.

### `wfi`: sleeping instead of spinning

When the buffer is empty, the echo loop runs `wfi` ("wait for interrupt"): the CPU sleeps until the next interrupt (a key or a timer tick). This is how real OSes save power when idle. On Windows it's the "System Idle Process".

## 5.8 Power off (`kernel/power.c`)

QEMU `virt` has a "test device" at `0x00100000`. Writing `0x5555` exits QEMU (power off), and `0x7777` reboots. It's registered as device `power0`: **Ctrl-D** in the echo loop writes the `POWER_COMMAND_OFF` command to it. It powers off only the KnocOS virtual machine, not your real computer.

## 5.9 Device abstraction (`kernel/device.c`)

**The problem it solves:** without it, the kernel hard-codes every device: the trap handler says "IRQ 10 = UART", `main.c` starts the UART by hand, and the echo loop calls UART functions directly. With 10 devices that becomes a mess, and adding one means editing many files.

**The idea:** every driver has **the same shape**, a `device_t` struct:

```c
typedef struct device {
    const char *name;                    // "uart0"
    uint32_t irq;                        // 10, or DEVICE_NO_IRQ
    int     (*init)(struct device *dev);
    void    (*interrupt)(struct device *dev);
    int64_t (*read)(struct device *dev, void *buffer, uint64_t length);
    int64_t (*write)(struct device *dev, const void *buffer, uint64_t length);
    int ready;
} device_t;
```

The `(*init)` fields are **function pointers**: each driver plugs in its own functions. The kernel calls `dev->read(...)` without knowing which driver it's talking to. This is C's version of an "interface".

```text
driver file (uart.c)                   device table (device.c)          the rest of the kernel
  static device_t uart_device = {        [0] uart0   IRQ 10  ready       device_find("uart0")
    .name = "uart0", .irq = 10,   ──►    [1] power0          ready  ◄──  device_read / device_write
    .init = ..., .read = ...  }          ...                             device_handle_irq(irq)
  uart_register()
```

**Boot:** each driver registers → `device_init_all()` runs every `init`, enables its IRQ in the PLIC, and logs `Device ready`.

**Interrupt:** PLIC claim → `device_handle_irq(irq)` finds the owner → calls its `interrupt` → PLIC complete. The trap handler never needs to change for a new device.

**The early console exception:** `log_info` and `panic` still call `uart_putc` directly. Crash messages must work before the device system starts, or even if it's broken. Linux does the same thing ("early console").

**Windows comparison:** this is the Windows driver model. Every driver has the same entry points, `ReadFile`/`WriteFile` work on any device, and Device Manager lists them all. Adding a device = one new driver file + one register call.

---

## 5.10 The disk: virtio-blk (`kernel/virtio_blk.c`)

### What it is

A **virtual hard disk**. QEMU turns the file `disk.img` on your PC into a disk that KnocOS can read and write. **Windows comparison:** the `.vdi` / `.vmdk` file behind a VirtualBox VM's C: drive.

- **virtio** = a standard, simple interface for virtual devices (disk, network, GPU, input all use it)
- **blk** = block device: storage made of numbered, fixed-size **sectors** (512 bytes)
- `disk.img` is 1 MiB = 2048 sectors. `make run` creates it with `Hello from the host!` in sector 0

### Character devices vs block devices

`device_read(uart0, buffer, 5)` means "give me the next 5 bytes" (a **stream**). A disk needs "give me **sector 7**", which a stream API can't express. So the device model has two kinds, like Linux and Windows:

| Kind | Example | API | Unit |
|---|---|---|---|
| Character | `uart0` | `device_read` / `device_write` | a stream of bytes |
| Block | `disk0` | `device_read_block` / `device_write_block` | numbered 512-byte blocks |

### Finding and starting the device

virtio-mmio slot 0 is at `0x10001000` (IRQ 1). The driver checks:

| Register | Expected | Meaning |
|---|---|---|
| Magic value | `0x74726976` | The ASCII letters `"virt"`: this is a virtio device |
| Version | 2 | Modern virtio (QEMU needs `-global virtio-mmio.force-legacy=false`) |
| Device ID | 2 | A block device (1 = network, 16 = GPU, ...) |

Then comes the **status handshake**, where the driver and device agree step by step:

```text
ACKNOWLEDGE  "I see you"
DRIVER       "I have a driver for you"
features     "I only need VIRTIO_F_VERSION_1"
FEATURES_OK  "agreed"             (the driver checks the device accepted it)
queue setup  (below)
DRIVER_OK    "ready to work"
```

### The virtqueue: a shared to-do list

The driver and the device share memory. Three rings, each in its own page:

```text
descriptor table   [0] header  → [1] 512-byte data → [2] status byte
  (what and where)     addr, len, flags (NEXT = chained, WRITE = device writes here)

available ring     driver → device: "request starting at descriptor 0 is ready"   (idx++)

used ring          device → driver: "request finished"                             (idx++)
```

### One sector read, step by step

```text
1. Fill the header: type = IN (read), sector = 7
2. Chain 3 descriptors: header → data buffer (device WRITEs it) → status byte
3. Put descriptor 0 in the available ring, then idx++          (fence before and after)
4. Write QUEUE_NOTIFY: "you have work"
5. Sleep on a wait queue (v0.12; before that: wfi). Other processes run
       QEMU reads sector 7 of disk.img into our buffer, writes status = 0,
       moves the used ring, raises IRQ 1
6. IRQ 1 → PLIC → trap → device_handle_irq(1) → disk0's interrupt handler
       acknowledges the interrupt, sees the used ring moved → request_done = 1,
       process_wake() → our process is READY again
7. Wake up, check status == 0 (OK), copy the 512 bytes to the caller
```

It's the same interrupt path as the keyboard. **`trap.c` and `plic.c` didn't change at all** to add the disk, which proves the device abstraction works.

### Details worth understanding

- **Memory fences** (`__sync_synchronize()`): the CPU and compiler may reorder memory writes. The device must see the descriptors *before* it sees `idx` change, so a fence forces that order. Shared memory with a device almost always needs fences.
- **Physical addresses:** the device knows nothing about our page tables. It needs **physical** addresses. The queue pages and the driver's own buffers are identity-mapped (virtual = physical), but a heap buffer at `0x2000000000` isn't. So the driver copies through its own sector buffer, and callers can pass any kernel buffer.
- **One request at a time:** simple and correct for now. A faster driver would keep several requests in flight (the queue has 8 slots).

### Proof that data survives

Sector 1 holds a **boot counter** (`"KNOCBOOT"` + a number). Every boot reads it, adds 1 and writes it back. `make test` boots twice on the same test disk and checks it goes from 1 to 2. You can also see it from Linux:

```bash
xxd -s 512 -l 16 disk.img
00000200: 4b4e 4f43 424f 4f54 0200 0000 0000 0000  KNOCBOOT........
```

That's your kernel writing to a real file on your PC.

---

## 5.11 Processes and the AI-aware scheduler (`kernel/process.c`)

### What a process is

A **process** is one running task: its own stack, its own saved registers, and a record in the **process table** (like one row in Task Manager). Right now they're **kernel threads**: they all share the kernel's memory. Separate protected memory comes with user mode.

```c
pid, name            identity
process_class        INTERACTIVE / AI_AGENT / NORMAL / BACKGROUND / IDLE
state                READY / RUNNING / SLEEPING / EXITED
context              saved ra, sp, s0–s11
stack                16 KiB from kmalloc
cpu_ticks, vruntime  CPU time used, weighted virtual runtime
```

### The context switch: the magic trick

One CPU can only run one thing. To "run many", the kernel keeps swapping which one is on the CPU:

```text
context_switch(&old->context, &new->context):
  save    old's ra, sp, s0–s11 into old->context
  load    new's ra, sp, s0–s11 from new->context
  ret     → "returns" into wherever the NEW process was when it was switched out
```

**Why only `ra`, `sp` and `s0`–`s11`?** `context_switch` is called like a normal function. By the calling convention, a function may destroy `t` and `a` registers anyway, so only the **saved** registers (plus `ra` and `sp`) need keeping.

**How a brand-new process starts:** its context is set up by hand with `ra = process_trampoline` and `sp = top of its new stack`. The first time it's switched to, `ret` jumps to the trampoline, which turns interrupts on and calls the process's function.

### Preemption: the timer takes the CPU back

```text
every 10 ms: timer → M-mode → SSIP → supervisor_trap (on the current process's stack)
  → timer_tick() → scheduler_tick()
       charge 1 tick to the current process
       wake any SLEEPING process whose wake_tick has come
       time slice used up?  → schedule() → context_switch to the next process
```

A process in `while (1) {}` never gives up the CPU, but the timer interrupts it anyway. That's **preemptive multitasking**, and the self-test proves it with three workers that never yield.

**Two things that had to be fixed for this to be safe:**
1. **M-mode got its own stack** (`mscratch`). Process stacks live in the heap at `0x9000xxxx`, a *virtual* address. M-mode doesn't use page tables, so pushing onto a process stack from M-mode would hit a physical address with no RAM there. Now M-mode always swaps to its own stack.
2. **The trap handler saves `sepc` and `sstatus`.** These are CPU registers, not memory. If process A is switched out inside a trap, process B's traps overwrite them. When A comes back, the handler restores its own copies before `sret`, so A returns to the right place.

### The AI-aware scheduler

| Class | Rule | Slice | For |
|---|---|---|---|
| INTERACTIVE | Always first | 1 tick | Keyboard, console |
| AI_AGENT | Weight 60 | 3 ticks | Agentic AI tasks |
| NORMAL | Weight 30 | 2 ticks | Regular programs |
| BACKGROUND | Weight 10 | 1 tick | Small NNs, maintenance |
| IDLE | Only when nothing else is ready | | pid 0 (`kernel_main`), `wfi` |

**Weighted sharing with virtual runtime:** each tick a process runs, `vruntime += 600 / weight`:

```text
AI_AGENT    +10 per tick     → runs 6 ticks for every 1 of BACKGROUND
NORMAL      +20 per tick
BACKGROUND  +60 per tick
The scheduler always picks the READY process with the LOWEST vruntime.
```

Worked example, all three busy, starting at 0 (ties go to the higher class):

```text
agent runs 3 → 30   normal runs 2 → 40   bg runs 1 → 60   agent 3 → 60
normal 2 → 80       agent 3 → 90         bg 1 → 120       normal 2 → 120   agent 3 → 120
over this cycle: agent 12 ticks, normal 6, background 2  =  60% / 30% / 10%
```

**Why not "AI always first"?** Absolute priority would **starve** everything else: a 10-minute agent task would freeze the keyboard. With `vruntime`, everyone's number keeps growing, so every process eventually has the lowest one and gets a turn. **No starvation, built in.** INTERACTIVE jumps the queue, but it only runs in tiny bursts, so the agent barely notices. And a process that wakes up (or is new) starts at the current lowest `vruntime`, so it can't claim a huge burst for the time it was asleep.

**Real-world comparison:** Linux's scheduler (CFS/EEVDF) uses the same virtual-runtime idea with weights from `nice` values. macOS has QoS classes much like this table. Windows has priority classes plus a boost for the foreground window. KnocOS adds a dedicated **AI_AGENT** class.

### Sleeping and the idle process

- `process_sleep(ticks)` marks the process SLEEPING with a `wake_tick`. `scheduler_tick()` wakes it when that tick arrives. If it's INTERACTIVE, it preempts whatever is running right away, which is why the test measures a **0-tick** wake latency.
- **pid 0 is `kernel_main` itself.** After boot it becomes the **idle process** and runs `wfi`. The scheduler picks it only when nothing else is ready.

### Interrupts during scheduling

`schedule()` must never be interrupted halfway (by a timer tick that also calls `schedule()`). So every path into it runs with interrupts **off**: trap handlers turn them off automatically, and `process_yield`/`sleep`/`create` turn them off with `csrrc sstatus` and restore them afterwards.

### Known limits (next steps)

- ~~**No locks yet**~~: fixed in v0.10 (spinlocks on the heap and page allocator, 4.7)
- ~~**The console polls** every 10 ms~~: fixed in v0.12 (wait queues, 5.15)
- ~~**Disk requests** wait with `wfi`~~: fixed in v0.12, the process sleeps and others run (5.15)

---

## 5.12 The AI space: AI that survives kernel crashes (`kernel/aispace.c`)

### The idea

If the AI lived inside the kernel, a kernel crash would kill it too. So the AI gets **its own CPU core** and **its own memory that the kernel physically cannot touch**. When the kernel dies, the AI keeps running, diagnoses the problem and recovers. **Windows comparison:** VBS / the Secure Kernel (a space the normal Windows kernel can't access). Apple's Secure Enclave and the management processors in PCs work on the same principle.

```text
core 0: kernel (S-mode, paging on)          core 1: AI space (M-mode, physical addresses)
   can crash or freeze                         never touched by the kernel
          │                                               ▲
          └───────────── mailbox (shared page) ───────────┘
            heartbeat, current process, crash details, last_kernel_pc
```

### Two cores

QEMU runs with `-smp 2 -m 2G`. **Both cores start at `_start`** at the same moment. `boot.S` reads `mhartid` (which core am I?): core 1 jumps to `aispace_boot`, gets its own stack in the AI region and calls `aispace_main()`, which never returns. Core 0 boots the kernel as before.

### PMP: hardware memory protection the kernel can't undo

**PMP** (Physical Memory Protection) is a set of M-mode registers that say which physical addresses lower modes may access. The kernel runs in S-mode, so **it cannot change them**. Core 0 sets two entries at boot:

```text
entry 0: 0x90000000–0x9FFFFFFF (256 MiB)  permissions: none   ← the AI space
entry 1: everything                       permissions: RWX
the lowest-numbered matching entry wins → the AI space is blocked, the rest is allowed
```

The address encoding is called **NAPOT** ("naturally aligned power of two"): `pmpaddr = (base | (size/2 - 1)) >> 2`. For a 256 MiB region at `0x90000000` that's `0x25FFFFFF`. The base must be aligned to the size, which is why the AI space sits at `0x90000000` (256 MiB aligned). Until v0.9.0 it was 16 MiB at `0x87000000`.

PMP is **per core**. Core 1's own PMP isn't set, and PMP doesn't restrict M-mode unless it's locked, so the AI space can see everything, including the kernel's memory. The protection is **one-way**: the AI can inspect the kernel, but not the other way around.

**Proof:** at boot, the kernel does a **probe read** of `0x90000000` (a load that returns an error instead of crashing, `trap_probe_read()`). It must fail with a *load access fault*, and `make test` checks that it does.

### Keeping the AI code in the AI space

The linker script puts `aispace.o`'s code, data and stack at `0x90000000` (`EXCLUDE_FILE(*aispace.o)` keeps them out of the kernel's sections). `aispace.c` is **self-contained**: its own UART output, clock reading and a small polled disk driver. It calls **no kernel function**, because kernel code could be the thing that's broken. `nm -u kernel/aispace.o` lists only `guardian_mailbox` and linker symbols (addresses such as `kernel_start` and `_start`, needed for the warm restart), never a kernel function.

### The mailbox

A shared structure in its own linker section. The AI space clears it when it starts.

| Field | Written by | Used for |
|---|---|---|
| `heartbeat`, `uptime_ticks` | Kernel, every tick | Freeze detection |
| `current_pid`, `current_name` | Kernel, every process switch | "Which process was running?" |
| `last_kernel_pc` | **Core 0's M-mode timer**, every tick | "Where is the kernel stuck?" This works **even when the kernel is frozen**, because M-mode timer interrupts can't be blocked by the kernel |
| `scause`, `sepc`, `stval`, `ra`, `sp`, `message`, `crash_type` | Kernel, on a trap or panic | The crash details |
| `kernel_state` | Kernel | `PANICKED` tells the AI "I'm dead, take over" |
| `watch_enabled` | Kernel, when boot is finished | The AI only watches for freezes once the kernel is running normally |

### What the AI does

```text
every 10 ms:
  kernel_state == PANICKED?            → handle crash
  heartbeat unchanged for 2 s + watch? → handle freeze

handle:
  collect   details from the mailbox
  diagnose  rules (Tier 0) → a sentence
  save      reset the disk, write the report with its own polled driver
  act       reboot / safe mode / halt, depending on the crash streak
```

**Why a polled driver?** The kernel's disk driver waits for an interrupt that goes to core 0's kernel, which is dead. The AI space instead **polls**: "is the used ring updated yet?" in a loop, with a timeout. Windows does the same when it writes a crash dump after a blue screen.

### The black box on disk

The last 8 sectors of `disk0`:

```text
sector N-8   header: magic, total_crashes, reported_crashes, consecutive_crashes
sector N-7   report slot 1  ┐
sector N-6   report slot 2  │ reports go round-robin: crash #5 overwrites slot 1
sector N-5   report slot 3  │
sector N-4   report slot 4  ┘
```

On the next boot, `guardian_boot_report()` (kernel side) prints every report newer than `reported_crashes`, then marks them reported.

### Recovery and boot-loop protection

| Crashes in a row | Action (v0.8.0) | Action (v0.9.0, see 5.13) | Windows equivalent |
|---|---|---|---|
| 1–2 | Reboot | Warm kernel restart | Automatic restart after a BSOD |
| 3 | Reboot into safe mode | Warm restart into safe mode | Safe Mode |
| 4+ | Halt | Halt the kernel, AI stays online | Automatic Repair instead of looping forever |

The `guardian` background process resets the streak after 60 s without a crash.

### Reboots don't reset everything: two lessons

A **warm reboot** (QEMU's reset, or pressing a reset button) restarts the CPU but **doesn't clear RAM or every CPU register**:
- **`.bss` kept old values.** C assumes uninitialized globals start at zero, but nothing cleared them: the first boot only worked because RAM started zeroed. Now `boot.S` clears `.bss` explicitly.
- **`satp` kept paging on.** The new kernel started with the *old* page table active, then overwrote that same memory while building new tables, and froze. Now `boot.S` sets `satp = 0` and clears the interrupt registers.

A kernel must **never assume the hardware state is clean at boot**.

### Where the real AI plugs in

The diagnosis is **rules** today. The same function (`ai_diagnose`) is where the **small NN crash classifier** goes in v0.15. It will be trained on reports produced by breaking KnocOS on purpose, like the Ctrl-F/P/W test keys. Later, a small LLM will explain crashes in plain language and choose among the same safe actions.

## 5.13 Fault containment and warm kernel restart (v0.9.0)

In v0.8.0 **every** crash was fatal: a bad pointer in the console process took the whole kernel down, and the AI's only fix was rebooting the machine, which restarted the AI too. v0.9.0 fixes both.

### Fault containment: kill the process, not the kernel

**Linux comparison:** an "oops". When a driver or kernel thread faults, Linux kills the task that was running and keeps going. **Windows comparison:** an app crash ("X has stopped working") versus a blue screen.

The hard question is: **when is it safe to keep running?** If the crash happened while the kernel was in the middle of changing a shared structure (the process table, the heap), continuing would run on broken data. KnocOS uses a simple rule:

| Where the fault happened | `sstatus.SPIE` | Decision |
|---|---|---|
| A process running normally | 1 (interrupts were on) | **Contain**: stop only this process |
| Code that turned interrupts off (a critical section) | 0 | Kernel crash |
| An interrupt handler | 0 (handlers run with interrupts off) | Kernel crash |
| Boot code or the idle process | (any) | Kernel crash |

**Why `SPIE`?** On a trap, the CPU copies `SIE` (interrupts on?) into `SPIE` and turns `SIE` off. So `SPIE` tells the handler whether interrupts were on **when the fault happened**. KnocOS only turns interrupts off around code that edits shared structures, so "interrupts were on" means "no shared structure was half-edited".

Containing the fault is short: `process_crash()` marks the process `CRASHED`, saves `scause`/`sepc`/`stval` and the driver it was in, and calls `schedule()`. The process never runs again, and its trap frame is simply abandoned. This is the same "switch inside a trap" trick the timer preemption already uses.

### The AI decides the fix

```text
trap handler      [OOPS] ... stopping only this process          (fast, no decisions)
guardian process  every 100 ms: a new CRASHED process? → mailbox: fault_seq++
AI space          sees fault_seq != verdict_seq → diagnose → verdict_action, verdict_seq = fault_seq
guardian process  applies the verdict: process_restart() and/or device_disable()
```

The **sequence numbers** make it a safe conversation between two cores without locks: the kernel writes all the fields **first** and bumps `fault_seq` **last** (with a memory barrier in between), so when the AI sees the new number, the fields are complete. The AI answers the same way.

The AI's rules (Tier 0): restart the process, unless it already crashed 3 times ("leave it stopped", like systemd's `StartLimitBurst`). If the crash happened **inside a driver**, disable that driver too.

### Which driver was running?

Every call into a driver goes through `device.c`, so it's the perfect place to track it: `process_driver_enter(name)` before the call and `process_driver_leave(previous)` after. The name is stored **per process**, because a process can be switched out while inside a driver (the disk driver waits for its interrupt), and interrupt handlers save and restore it like a stack. When a crash happens, `process_current_driver()` answers "which driver?". The `faulty0` test driver (Ctrl-X) proves it: the AI disables it, and a second Ctrl-X only prints `faulty0 is disabled`.

The AI keeps its list of disabled drivers **in its own memory** and publishes it in the mailbox. That list survives a warm kernel restart (the kernel forgets everything, the AI doesn't), so `device_init_all()` skips the driver: `Device disabled by the AI space: faulty0`.

### Warm kernel restart: the AI never stops

A reboot resets **both** cores. To keep the AI running, only core 0 may restart. That needs three things:

**1. A clean copy of the kernel.** When core 0 restarts at `_start`, the kernel's `.data` still has the values from the crashed run (`next_pid`, device pointers...), and its code might be damaged (the kernel maps its code `RWX`, so a bad pointer can overwrite it). A QEMU reboot reloads the ELF file, but a warm restart doesn't. So at boot the AI space copies `kernel_start..kernel_image_end` (code + read-only data + data, 38 KiB) into its protected memory (`aispace_snapshot` in the linker script). `.bss` isn't copied: `boot.S` clears it anyway.

**2. Taking the copy before the kernel changes anything.** Both cores start at the same moment. If core 0 ran first, it would change `.data` before the copy. So core 0 waits at the very top of `boot.S`:

```text
core 0: boot_ack = 0; boot_request = nonce (mtime | 1); wait until boot_ack == nonce (1 s timeout)
core 1: copy the kernel; then, in its main loop: if boot_request != boot_ack: boot_ack = boot_request
```

The nonce matters because RAM keeps old values across a QEMU reset: an old `boot_ack` from the previous boot must never look like "go ahead". Core 0 clears `boot_ack` first, and only the AI space's main loop writes it, which only starts **after** the copy. These two words are the first fields of the mailbox, because `boot.S` needs fixed offsets (`MAILBOX_BOOT_REQUEST`, `MAILBOX_BOOT_ACK`, checked with `_Static_assert`).

**3. Stopping core 0 from outside.** A frozen kernel with interrupts off won't stop politely. But **machine-mode interrupts can't be blocked by S-mode**, the same reason `last_kernel_pc` works during a freeze. The AI space writes `1` to core 0's **MSIP** register in the CLINT (`0x02000000`), which is an inter-processor interrupt:

```text
core 1: core0_release = 0, MSIP[0] = 1
core 0: machine_trap (mcause = 0x8000000000000003) → aispace_park_core0()
        MSIP[0] = 0, core0_parked = 1, spin until core0_release
core 1: check the code, save the black box, copy the clean kernel back,
        reset the mailbox (keeps the restart count and disabled drivers), core0_release = 1
core 0: fence.i → jump to _start → a fresh kernel boots
```

`aispace_park_core0()` is **AI space code running on core 0**. PMP isn't locked, so it doesn't restrict M-mode, and core 0 can run it. This matters: the parking loop must not live in kernel memory that the AI is about to overwrite, or that might be corrupted.

**`fence.i`:** core 1 wrote new instructions into memory. A CPU may still hold the old ones in its instruction cache, and `fence.i` tells core 0 to fetch them again.

**Fallback:** if core 0 doesn't park within 0.5 s (for example, stuck in an M-mode loop), the AI reboots the whole machine as in v0.8.0.

### The kernel code check

The clean copy gives the AI something new to diagnose with: it compares the running kernel code (`kernel_start..kernel_code_end`) with the clean copy **before** restoring it. Ctrl-O zeros the first 16 bytes of `log_info()` and calls it:

```text
[AI] Kernel code check: 14 bytes differ from the clean copy (code corrupted)
[AI] Diagnosis: Kernel code was overwritten in memory: a bad pointer wrote over it. The clean copy fixes it.
```

(14, not 16, because 2 of those bytes were already zero.) A plain restart would crash again right away, since `log_info()` is the first function `kernel_main` calls. After the restore the kernel boots normally. That's the proof that the restore works.

### Hardware that survives a kernel restart

A reboot resets devices, but a warm restart doesn't, so the new kernel finds hardware in whatever state the old one left:

| Leftover | Fix |
|---|---|
| An interrupt the crashed kernel **claimed** but never **completed** in the PLIC: that IRQ would never fire again | `plic_enable()` completes the IRQ right after enabling it |
| PLIC enable bits for a driver that's now disabled | `plic_init()` clears all enable bits |
| The disk's virtqueue points at the old kernel's (or the AI space's) memory | `virtio_blk_init()` already resets the device (`status = 0`) |
| `sstatus.SIE` still on from the old kernel | `boot.S` clears it |

Same lesson as v0.8.0's warm-reboot bugs: **never assume the hardware is clean at boot**.

### Known limits

- The disabled-driver list lives in AI memory: a full power cycle forgets it (a filesystem will store it, v0.12)
- Both cores print to the same UART with no lock, so lines can mix if both print at once (fixed in v0.10 with a line lock, see 4.7)
- Kernel processes are kernel threads: a "contained" bug may already have damaged kernel memory. User programs (v0.11, see 5.14) can't, because they run in U-mode

## 5.14 User mode and system calls (v0.11.0)

### Why user mode?

Up to v0.10, every process ran in S-mode, the kernel's own privilege level. A bug in any process could overwrite anything: the heap, the page tables, other processes. Fault containment helped, but only **after** the damage. **User mode** stops the damage from happening at all.

RISC-V has three privilege levels, and KnocOS now uses all of them:

| Mode | Who | Can touch |
|---|---|---|
| M (machine) | Boot code, the timer handler, the AI space | Everything (PMP limits the others) |
| S (supervisor) | The kernel | Pages without the U bit, CSRs like `satp` |
| **U (user)** | **Programs** | **Only pages with the U bit.** No CSRs, no devices |

A program that touches anything else gets a page fault, and the kernel stops it. **Windows comparison:** ring 3 (apps) vs ring 0 (the kernel). An app crash shows "X has stopped working", never a blue screen.

### One page table per program

Each program gets its own root page table (`vm_user_create()`):

```text
root slot 0        MMIO (UART, PLIC, virtio...)   ┐
root slots 2..9    RAM, identity megapages        │ copied from the kernel root:
root slot 128      kernel heap                    ┘ the SAME tables, no U bit
root slots 64..127 0x1000000000..0x1FFFFFFFFF     ← private to this program, U bit
```

Copying the kernel's root entries means the kernel stays mapped while it handles a trap, so there's no need to switch page tables on every system call. Without the U bit, the program still can't read any of it. Linux did exactly this for many years (until the Meltdown CPU bug, which needed the kernel to be fully unmapped).

On every process switch, the scheduler loads the next process's `satp` (`vm_switch()`) and flushes the TLB with `sfence.vma`, because the same virtual address now means different memory.

### Getting into U-mode (and back)

There's no "jump to U-mode" instruction. The kernel pretends it's **returning** from a trap: `sret` goes to `sepc` in the mode stored in `sstatus.SPP`. `user_enter()` sets `sepc` = the program's entry point, `SPP` = 0 (U-mode), `SPIE` = 1 (interrupts on afterwards), clears **every register** (so no kernel values leak into the program), and runs `sret`.

Coming back is the harder part. A trap from U-mode lands in `supervisor_trap`, but `sp` is the **program's** stack pointer, which could be anything. So KnocOS uses `sscratch`:

```text
sscratch = 0                 while the CPU runs kernel code
sscratch = kernel stack top  while the CPU runs a program

supervisor_trap:
  csrrw sp, sscratch, sp     swap them
  sp != 0?  → came from a program: sp is now a safe kernel stack,
              the program's sp is in sscratch (saved into the frame)
  sp == 0?  → came from the kernel: swap back, continue as before
```

On the way out, `sstatus.SPP` says where to return. If it's 0 (a program), `sscratch` gets the kernel stack top again before `sret`.

### System calls

A program asks the kernel for something with `ecall`. That's a trap with `scause = 8` ("environment call from U-mode"):

```text
program:  a7 = 1 (write), a0 = buffer, a1 = length, ecall
kernel:   syscall_handle(frame): switch on frame->a7, result → frame->a0, sepc += 4
program:  continues after the ecall, result in a0
```

`sepc += 4` matters: `sepc` points **at** the `ecall`, and returning there would call the kernel forever. The numbers, errors and capabilities are in one header (`kernel/syscall_abi.h`) shared by the kernel, the AI space and every program.

### Never trust a pointer from a program

`write(buf, len)` gives the kernel an address. The program could pass a **kernel** address, hoping the kernel reads its own secrets and prints them. The kernel can read kernel memory, so a naive `memcpy` would leak it. KnocOS checks every page **in the program's page table**: it must be a user page (U bit) with read permission, and the copy goes through the physical address found there. A bad pointer returns `E_FAULT`. The `badcall` program tests exactly this.

### Capabilities, quotas and the trace: first steps of intent security

- **Capabilities:** each built-in program has permission bits (`CONSOLE`, `SPAWN`, `MEMORY`). A call without its capability is refused (`E_PERM`), counted and logged: `[SECURITY] noperm (pid 8) called spawn without the SPAWN capability: denied`
- **Quotas:** 16 MiB for normal programs, 1 GiB for `AI_AGENT` programs. AI-aware scheduling now extends to memory. Memory is **zeroed** before a program gets it, so it can never see another program's old data
- **Trace:** the kernel records every program's last 8 system calls. When a program crashes, the AI space gets the trace and the number of forbidden calls:

```text
[AI]   last system calls: write, spawn, write  (forbidden: 1)
[AI] Security: it made 1 forbidden system call(s) before crashing: treated as suspicious
[AI] Action: leave spy stopped (suspicious program, not restarted)
```

This is the seed of goal.md's **intent-based security**: judge programs by *what they try to do*. Today it's one rule. The recorded traces are the kind of data the intent classifier NN will learn from (v0.15).

### Loading programs: ELF

Programs are normal ELF files built from `user/`: `crt0.S` (calls `main`, then `exit`), `ulib.c` (system call wrappers) and one `.c` file each, linked at `0x1000000000`. `elf_load()` reads the program headers and, for each `PT_LOAD` segment, allocates memory, copies the bytes, zeroes the rest (`.bss`) and maps it with the segment's permissions (code `R-X`, data `RW-`). There's no filesystem yet, so `kernel/programs.S` embeds the ELF files in the kernel with `.incbin`. v0.12 will load them from disk instead.

### Cleaning up

Every block a program gets (code, stack, `mem_alloc`) is recorded in its process. On `exit`, the kernel first switches back to the kernel page table (it can't free the table it's standing on), then frees the program's page tables and every block. The self-test checks that the number of free pages is exactly the same after running 5 programs, including the 256 MiB AI block.

## 5.15 Wait queues: sleeping until something happens (v0.12.0)

### Polling vs waiting

Up to v0.11, a process that had to wait **polled**: the console asked "is there a key?" 100 times a second, `process_wait()` checked every tick whether a program had ended, and the guardian looked for crashes every 100 ms. Worse, the disk driver waited with a `wfi` loop, which wasted the waiting process's whole time slice.

**Event-based waiting** turns it around: the process says "wake me when X happens" and sleeps. The code that makes X happen (usually an interrupt handler) wakes it. Like a phone that rings instead of checking it every 10 seconds.

### How it works

A **channel** is just an address that names the event: the keyboard buffer, the disk, a process slot.

```c
process_block(channel, timeout);   // state = BLOCKED, remember the channel, schedule()
process_wake(channel);             // every process BLOCKED on channel → READY
```

The classic trap is the **lost wake-up**:

```text
console: buffer empty?  yes
                               ← key arrives, interrupt: process_wake(&rx) → nobody sleeping yet!
console: process_block(&rx)       sleeps forever (well, until the next key)
```

The fix: check the condition and go to sleep **with interrupts off**. The interrupt can't run between the check and the sleep; it runs after the switch, when the process really is on the queue:

```c
irq_save();                       // interrupts off
while (buffer is empty)
    process_block(&rx_channel, 0);  // schedule() switches away with interrupts off
irq_restore();
```

**Waking up fast:** `process_wake()` just marks the process READY. If it's interactive (the console) or the CPU was idle, it also sets `resched_pending`, and `scheduler_preempt()` switches to it right after the interrupt. So a key press reaches the console at once, not at the next tick.

A **timeout** (in ticks) makes the scheduler tick wake the process anyway. The guardian uses it: it's woken **at once** by a crash, and otherwise every second for its other checks.

### Sleep locks

A spinlock (4.7) turns interrupts off, so it can't be held across a disk read that takes a while. A **sleep lock** can: a process that finds it taken **sleeps** on the lock's channel until the owner releases it. The disk driver has one. Before v0.12 there was a hidden bug: if a timer tick switched processes during a disk read, and the other process also used the disk, both used the same descriptors and buffer. It never happened only because no two processes used the disk at once.

**Crash safety:** a process that crashes can't release its locks itself. So each process remembers the sleep locks it holds, and `process_crash()` releases them. Otherwise one contained crash could freeze the disk for everyone.

### Proof

The self-test reads 200 sectors while a CPU-bound worker runs. When a read has to wait, the reader sleeps, and the worker's counter moves between reads, with no timer tick needed:

```text
[INFO] Wait queues verified: 200 disk reads, the reader slept 200 times and another process ran meanwhile
```

(Sometimes QEMU finishes a request before the driver even checks. Then there's nothing to wait for, and the process doesn't sleep. The test accepts that.)

## 5.16 KnocFS: files and folders on the disk (v0.12.0)

### Why a filesystem?

Before v0.12 the disk was 2048 numbered sectors, and programs were built into the kernel. An AI OS needs **files**: model weights, programs, notes, crash logs. A filesystem is the table of contents that turns "sectors 70000–72047" into `/models/qwen.gguf`. **Windows comparison:** NTFS on `C:`.

### The layout

```text
disk sectors   0 ─ 2047 │ 2048 ─────────────────────────────── N-9 │ N-8 ─ N-1
               test data │ KnocFS                                   │ AI black box

KnocFS (4 KiB blocks):
block 0         superblock: magic "KNOCFS01", sizes, where everything is
block 1..       free-block bitmap: 1 bit per block (1 = used)
next 32 blocks  inode table: 1024 inodes × 128 bytes (inode 1 = the root directory)
the rest        data blocks
```

An **inode** describes one file or directory: type, size, and where its data is. A **directory** is a file whose data is a list of 64-byte entries `{inode number, name}`. Looking up `/models/test-model.bin` means: root directory (inode 1) → find `models` → its inode → find `test-model.bin` → its inode.

### Extents: made for big model files

Many filesystems list every block of a file (ext2, FAT). KnocFS stores **extents**, `{start block, count}` pairs (like ext4, NTFS and XFS). A 1 GiB model copied onto the disk in one piece is **one extent**: 262,144 blocks in a row, which can be read back to back. An inode has room for 12 extents. When a file grows, KnocFS first tries to extend the last extent in place, and otherwise takes the first free run that's long enough.

### Safety details

- **New blocks are zeroed** (or fully written) before they belong to a file, so a file can never show another file's old data
- **One sleep lock** for the whole filesystem, since operations sleep during disk I/O
- The **black box area** at the end of the disk is outside KnocFS. `knocfs_mount()` checks that, so the AI space's crash reports are never overwritten

### Files for programs

Programs see files through **file descriptors** (small numbers): 0 = keyboard, 1 and 2 = screen, 3 and up = open files. This is the Unix design, and the same idea as Windows handles.

```c
int fd = open("/home/note.txt", O_WRITE | O_CREATE | O_TRUNC);
write(fd, "KnocOS remembers this", 21);
close(fd);
```

New capabilities `FILES_READ` and `FILES_WRITE` guard them. File reads go **directly into the program's pages**: the kernel finds each page's physical address in the program's page table (as in 5.14) and the filesystem reads into it. There's no extra copy, which matters when an AI program loads gigabytes of weights. `modelcheck` loads an 8 MiB test model in about 0.6 s under QEMU.

### Programs from the disk

`process_spawn()` now loads `/bin/<name>` from KnocFS. The copies built into the kernel are only a **fallback**, for example if the AI space disabled `disk0` after it crashed. The **kernel** decides a program's class and capabilities, not the file, so replacing `/bin/spy` can't give it more permissions.

### Getting files onto the disk from your PC

`tools/knocfs.py` is a Python version of the same format:

```bash
make reset-disk                                   # fresh 64 MiB disk (DISK_MB=4096 for more)
make put FILE=qwen.gguf DEST=/models/qwen.gguf    # copy a model onto the disk
make ls DIR=/models
```

### A bug found on the way

The `files` program printed empty error messages. The cause: the program's data segment started at `0x10000010e0`, **in the same page** as the end of its code. The loader mapped a fresh zeroed page for the data segment over the code page, wiping the program's text strings. The fix: the program linker script page-aligns the data segment, and `elf_load()` now **rejects** segments that share a page instead of silently mapping over one. Lesson: a loader must never assume the file it loads is well-formed.

---

## 5.17 The shell: knocsh (v0.13.0)

### What a shell is

A **shell** is the program you type commands into. It reads a line, works out what you mean, and asks the kernel to do it with system calls. On Linux it's `bash`, on Windows `cmd` or PowerShell. The important idea: **the shell is not part of the kernel**. It's an ordinary program, and it can only do what its system calls allow. `knocsh` is `/bin/knocsh`, in the INTERACTIVE class (so it always answers first), with every capability including the new `SYSTEM` one.

### The loop

```text
print the prompt  knoc:/home$
read keys until Enter (echo each one, Backspace erases)
split the line into words
first word is a built-in?  → do it (ls, cd, cat, ps, mem, ai...)
else is it /bin/<word>?    → spawn it, wait for it (or not, with &)
else                       → "unknown command"
```

The **current directory** exists only inside the shell. `cd home` just changes a string, and the shell turns relative paths into absolute ones (resolving `.` and `..`) before any system call. The kernel only ever sees absolute paths. Linux keeps the current directory in the kernel instead, because every program has one; KnocOS may do that later.

### Who gets the keyboard? The terminal layer

Two things want the keys: the kernel console (Ctrl-D, Ctrl-C, the test keys) and the shell (everything else). `kernel/tty.c` sits between them:

```text
UART interrupt → console process: control key? handle it : tty_input(c)
                                                              │
knocsh: read(0) ─── tty_read() sleeps on a wait queue until ──┘
```

This is a tiny version of what Unix calls a **tty** (from teletype). The program marked `PROGRAM_TERMINAL` becomes the terminal's **owner** when it starts. If the shell crashes and the AI restarts it, the new one becomes the owner again. While no shell runs (for example after `exit`), the console echoes keys itself, as before v0.13.

### Ctrl-C and the foreground program

When the shell runs a program and waits for it (`wait` system call), the kernel records that program as the **foreground** process. Ctrl-C reaches the console (the shell never sees it), and the console kills the foreground process. `wait` then returns `E_KILLED` and the shell prints `counter stopped (Ctrl-C)`. On Linux, Ctrl-C sends the signal `SIGINT`, which a program can catch. KnocOS just stops the program.

Killing had to become safer for this. A process killed while it sleeps inside a system call (for example in the middle of a disk read) could be holding the filesystem or disk lock. `process_kill` now releases its sleep locks (like a crash does) and wakes anyone waiting for it.

### Looking inside the system: the new system calls

Some things only the kernel knows: the process table, free memory, the devices, and the AI space's crash reports. Six new system calls expose them, all but `wait` behind the `SYSTEM` capability (ordinary programs can't list processes or kill them):

| Call | Used by |
|---|---|
| `wait(pid)` | Running a program in the foreground |
| `ps(i, info)` | `ps` |
| `kill(pid)` | `kill` (user programs only) |
| `sysinfo(info)` | `mem`, `ai` |
| `devinfo(i, info)` | `devices` |
| `crashinfo(i, info)` | `crashes`: the black box reports, with the AI's diagnosis and action |

The pattern is "give me item *i*": the program asks for 0, 1, 2... until it gets `E_NOTFOUND`, so the kernel never needs to know how big the program's buffer is.

### Safe mode gets a shell too

After 3 crashes in a row, KnocOS starts in safe mode with minimal services. The shell starts too, like Windows' "Safe Mode with Command Prompt", so you can look at what happened:

```text
knoc:/$ crashes
#3 TRAP in console (pid 6) after 5 s: Unhandled supervisor trap
   AI diagnosis: Bad pointer: the code accessed an address that is not mapped (stval).
   AI action: warm kernel restart into safe mode
knoc:/$ ai
Warm kernel restarts: 3
Drivers disabled by the AI: faulty0
Safe mode: on
```

### A bug caught on the way

After Ctrl-F crashes the console process, the AI restarts it, and the restarted console starts at the top of its code again, including "start the shell". That would have started a **second** shell while the first was still running, with two programs fighting over the keyboard. The console now starts a shell only when no program owns the terminal.

---

# Part 6: Engineering

## 6.1 The Makefile

- **Pattern rules** (`%.o: %.c`) compile any C file the same way.
- **`-MMD -MP`**: the compiler writes a `.d` file listing every header each `.c` file includes, and `make` reads them. Changing a header rebuilds exactly the files that use it.
- **`-Wall -Wextra -Werror`**: turns on many warnings and makes **any warning a build error**. Warnings often hide real bugs.
- **`-DKNOCOS_VERSION='"v0.8.0"'`**: the version from the `VERSION` file becomes a C string the kernel prints at boot.

## 6.2 Automated testing (`make test`)

`scripts/test.sh`:
1. Boots KnocOS in QEMU
2. Waits 2 s, types `knocos-echo-test` + Enter, then Ctrl-D
3. Checks that the output contains every self-test message, the echo and "Powering off"
4. Fails if there's any `[PANIC]`, or if QEMU doesn't exit within 15 s

**Why it matters:** every change can be verified in about 5 seconds. Production projects never rely on "I looked at the screen and it seemed fine".

## 6.3 Continuous integration

`.github/workflows/ci.yml` runs on GitHub's servers for every push: install the RISC-V toolchain and QEMU, `make`, `make test`. The badge at the top of the README shows green (pass) or red (fail).

## 6.4 Versioning

**Semantic versioning**: `MAJOR.MINOR.PATCH`. Below `1.0.0`, each minor version is a milestone:

| Version | Milestone |
|---|---|
| 0.1.0 | First boot |
| 0.2.0 | Physical + virtual memory |
| 0.3.0 | Kernel heap |
| 0.4.0 | Interrupts, traps, PLIC, keyboard, tests, CI |
| 0.5.0 | Device abstraction, Phase 4 complete |
| 0.6.0 | virtio-blk disk driver, permanent storage |
| 0.7.0 | Processes and the AI-aware scheduler |
| 0.8.0 | AI space: survives kernel crashes, black box, recovery |
| 0.9.0 | Fault containment, AI verdicts, warm kernel restart: the AI never stops |
| 0.10.0 | Big memory: device tree, buddy allocator, megapages, spinlocks, 256 MiB AI space |
| 0.11.0 | User mode, system calls, capabilities, quotas, system call trace for the AI |
| 0.12.0 | Wait queues, KnocFS filesystem, programs and models on disk |
| 0.13.0 | The shell knocsh, the terminal layer, Ctrl-C, system information calls |

To release: update `VERSION` and `CHANGELOG.md`, commit, then `git tag v0.13.0 && git push --tags`.

---

# Part 7: Debugging

## 7.1 Tools

| Tool | Command | Use |
|---|---|---|
| GDB | See README "Debugging" | Step through code, look at registers |
| objdump | `riscv64-unknown-elf-objdump -d knocos.elf` | Disassembly: find what's at an address |
| nm | `riscv64-unknown-elf-nm -n knocos.elf` | Symbol addresses, sorted |
| readelf | `riscv64-unknown-elf-readelf -l knocos.elf` | Segments and permissions |
| vm_debug | `vm_debug(address)` in C | Print a page table walk |
| page_debug | `page_debug()` in C | Print the used/free page map |

## 7.2 Reading a crash

```text
[TRAP] Store page fault
[TRAP] scause = 0x000000000000000F     ← 15 = store page fault
[TRAP] sepc   = 0x0000000080000784     ← the instruction that crashed
[TRAP] stval  = 0x0000000040000000     ← the address it tried to write
[TRAP] ra     = 0x000000008000077C     ← who called the crashing function
[TRAP] sp     = 0x00000000800080F0
[PANIC] Unhandled supervisor trap
```

How to investigate:
1. **What?** `scause` gives the type (see the table in the cheat sheet).
2. **Where?** Look up `sepc` in `objdump -d knocos.elf`, then find the surrounding function name.
3. **Which address?** `stval`. Is it mapped? Check with `vm_debug(stval)`.
4. **Who called it?** `ra` is the return address into the caller.

## 7.3 Bugs we hit and what they taught us

| Bug | Cause | Lesson |
|---|---|---|
| Timer test failed | The test waited with a loop count, but the timer runs on real time | Never measure time with loops. Use the clock (`rdtime`) |
| Kernel couldn't safely touch the timer after paging | The CLINT isn't in the page tables | After `satp` is set, every address must be mapped. Or use CSRs like `rdtime` |
| `stvec` handler never ran | `medeleg` was 0, so every exception went to M-mode | Traps go to M-mode unless delegated |
| Changes to headers or the linker script were ignored | The Makefile didn't know about them | Use automatic dependencies (`-MMD`) |
| Linker warning: RWX segment | Code and data were in one segment | Separate `R-X` and `RW-` with `PHDRS` |
| Timer constants in two places | `boot.S` hard-coded numbers | Share one header between C and assembly (`#ifndef __ASSEMBLER__`) |
| `undefined reference to memcpy` | Copying a struct makes GCC call `memcpy`, and bare metal has no C library | A kernel must provide `memcpy`/`memset` itself (`kernel/string.c`) |
| (avoided) M-mode writing to a process stack | Process stacks are virtual heap addresses, but M-mode uses physical addresses | Give M-mode its own stack with `mscratch` |
| (avoided) Returning to the wrong place after a switch | `sepc`/`sstatus` are CPU registers shared by every trap | Save them in the handler and restore before `sret` |
| False "freeze" right after a reboot | `.bss` was never cleared, so the mailbox kept "watching = on" from the previous boot | Clear `.bss` in `boot.S`; the AI space resets the mailbox |
| New kernel froze after a warm reboot | `satp` still had paging on from the previous boot, and the kernel overwrote its own live page table | Set `satp = 0` at boot. Never trust CPU state after a warm reset |

---

# Part 8: What Comes Next

## 8.1 The small NN runtime (v0.14.0)

Tensors and int8 math inside the AI space, loading its weights from `/models`, to replace the rule brain with a trained crash classifier (v0.15).

## 8.3 Toward the AI-OS

After the kernel foundation: filesystem, shell, the small neural network runtime, the LLM runtime, the agent, KnocNet and app compatibility. See `goal.md`. Every one of those depends on what's in these notes: memory for models, the scheduler for AI workloads, drivers for disk/network/GPU, and traps for security and self-diagnosis.

---

# Cheat Sheets

## Memory map (QEMU `virt`)

| Address | What | Mapped in Sv39 |
|---|---|---|
| `0x00100000` | Test device (power off / reboot) | ✅ 1 page |
| `0x02004000` | CLINT `mtimecmp` (hart 0) | ❌ (M-mode only) |
| `0x0200BFF8` | CLINT `mtime` | ❌ (read with `rdtime`) |
| `0x0C000000` | PLIC | ✅ 4 MiB |
| `0x10000000` | UART0 (IRQ 10) | ✅ 1 page |
| `0x10001000` | virtio slot 0: `disk0` (IRQ 1) | ✅ 1 page |
| `0x80000000` | RAM start, kernel, stack, page info | ✅ identity, 2 MiB megapages |
| `0x90000000` | AI space (256 MiB) | Mapped, but **PMP blocks the kernel** |
| `0xBFE00000` | Device tree (placed by QEMU) | ✅ reserved |
| `0x100000000` | RAM end with `-m 2G` (from the device tree) | |
| `0x2000000000` | Kernel heap (virtual) | ✅ grows on demand |

## CSR cheat sheet

| CSR | Mode | Meaning |
|---|---|---|
| `mstatus` / `sstatus` | M / S | Global state: interrupt enable (MIE/SIE), previous mode (MPP/SPP) |
| `mtvec` / `stvec` | M / S | Trap handler address |
| `mepc` / `sepc` | M / S | Where the trap happened, and where `mret`/`sret` returns |
| `mcause` / `scause` | M / S | Why the trap happened |
| `stval` | S | Extra info (bad address, bad instruction) |
| `mie` / `sie` | M / S | Which interrupts are enabled |
| `mip` / `sip` | M / S | Which interrupts are pending |
| `medeleg` / `mideleg` | M | Which exceptions / interrupts go straight to S-mode |
| `mcounteren` | M | Which counters lower modes may read (`TM` = `rdtime`) |
| `satp` | S | Paging mode + root page table address |
| `pmpaddr0` / `pmpcfg0` | M | Physical memory protection |

## `scause` values

| Interrupt (top bit 1) | | Exception (top bit 0) | |
|---|---|---|---|
| 1 | Supervisor software (forwarded tick) | 0 | Instruction address misaligned |
| 5 | Supervisor timer | 1 | Instruction access fault |
| 9 | Supervisor external (PLIC) | 2 | Illegal instruction |
| | | 3 | Breakpoint (`ebreak`) |
| | | 5 / 7 | Load / store access fault |
| | | 8 | `ecall` from U-mode |
| | | 12 / 13 / 15 | Instruction / load / store page fault |

## Commands

```bash
make            # build
make run        # run (Ctrl-D powers off, Ctrl-A X force-quits)
make test       # automated test → RESULT: PASS
make clean      # delete build files
make size       # kernel size
make pages      # page layout
make reset-disk # recreate disk.img
xxd disk.img | head                                 # look at the disk from Linux
riscv64-unknown-elf-objdump -d knocos.elf | less    # disassembly
riscv64-unknown-elf-nm -n knocos.elf                # symbols
```

## Glossary

| Term | Meaning |
|---|---|
| **Bare metal** | Running with no OS underneath |
| **Foreground process** | The program the shell is waiting for; Ctrl-C stops it |
| **Shell** | The program you type commands into (`knocsh`, `bash`, `cmd`) |
| **Terminal (tty)** | The layer that hands keyboard input to the program that owns it |
| **Extent** | A run of consecutive disk blocks `{start, count}` holding part of a file |
| **File descriptor** | A small number a program uses for an open file (0 keyboard, 1–2 screen) |
| **Inode** | The on-disk record of one file: type, size and where its data is |
| **Sleep lock** | A lock a process can hold while it sleeps; waiters sleep instead of spinning |
| **Wait queue** | Processes sleeping until an event (a channel) wakes them |
| **Buddy allocator** | Page allocator with power-of-two blocks that split on allocation and merge with their "buddy" on free |
| **Capability** | A permission bit a program must have to use a system call (`CONSOLE`, `SPAWN`, `MEMORY`) |
| **Device tree** | Binary description of the machine (RAM, CPUs, devices) passed by the firmware |
| **ELF** | The executable file format of KnocOS programs (and of Linux) |
| **Megapage** | A 2 MiB mapping made by one leaf entry in a level-1 page table |
| **Spinlock** | A lock that waits by looping on an atomic instruction |
| **System call** | A program's request to the kernel: `ecall` with a number in `a7` |
| **U-mode** | User mode: the CPU's lowest privilege level, where programs run |
| **CLINT** | Core-local interruptor: the timer (and software interrupts) hardware |
| **Coalescing** | Merging neighbouring free heap blocks |
| **Context switch** | Saving one program's registers and loading another's |
| **Idle process** | What runs when nothing else can: pid 0, sleeps with `wfi` |
| **Preemption** | The timer forcibly takes the CPU from a process |
| **Process** | One running task with its own stack and saved registers |
| **Starvation** | A process never getting CPU time because others always win |
| **Time slice** | How long a process may run before the scheduler reconsiders |
| **vruntime** | Virtual runtime: CPU time scaled by weight. Lowest runs next |
| **CSR** | Control and status register: CPU configuration |
| **ELF** | Executable file format used on Linux and bare-metal RISC-V |
| **Black box** | Crash reports saved to disk so the next boot (and the AI) knows what happened |
| **Hart** | Hardware thread: a RISC-V CPU core |
| **Heartbeat** | A counter the kernel bumps every tick. If it stops changing, the kernel is frozen |
| **NAPOT** | "Naturally aligned power of two": the PMP address format for aligned regions |
| **Safe mode** | A boot with minimal services, used after repeated crashes |
| **Warm reboot** | Restarting the CPU without clearing RAM or all registers |
| **Watchdog** | Something that checks a heartbeat and acts when it stops |
| **Identity mapping** | Virtual address = physical address |
| **IRQ** | Interrupt request number of a device |
| **MMIO** | Memory-mapped I/O: devices appear as addresses |
| **Page** | 4 KiB block of memory |
| **Page fault** | Access to an address that isn't mapped (or not allowed) |
| **PLIC** | Platform-level interrupt controller: routes device interrupts |
| **PMP** | Physical memory protection: M-mode's access rules for lower modes |
| **PTE** | Page table entry |
| **Ring buffer** | Circular array with read and write indexes |
| **Sv39** | RISC-V 3-level paging with 39-bit virtual addresses |
| **TLB** | CPU cache of address translations |
| **Trap** | Exception or interrupt: the CPU jumps to a handler |
| **UART** | Serial port chip: text in and out |
| **Block device** | Storage made of numbered fixed-size blocks (a disk) |
| **Character device** | A stream of bytes (a serial port, a keyboard) |
| **Memory fence** | An instruction that stops the CPU from reordering memory accesses across it |
| **Sector** | One 512-byte block of a disk |
| **virtio** | Standard simple interface for virtual devices |
| **Virtqueue** | Ring buffers in shared memory used to send requests to a virtio device |
| **volatile** | C keyword: "this memory can change behind the compiler's back" |
| **wfi** | Wait for interrupt: sleep until something happens |
