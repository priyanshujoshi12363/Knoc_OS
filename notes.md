# KnocOS Notes: From Basics to Advanced

These notes explain **how KnocOS works and why it is built this way**, starting from zero. Read them in order the first time. Later, use them as a reference.

Each part links theory to the actual KnocOS code, so you can open the file and see the idea in practice.

> If you understand these notes, you understand every line of KnocOS.

---

## Contents

- **Part 1: Basics:** what an OS is, bare metal, QEMU, RISC-V
- **Part 2: Building and booting:** toolchain, linker script, `boot.S`, privilege modes
- **Part 3: Talking to hardware:** memory-mapped I/O, UART, logging
- **Part 4: Memory:** physical pages, virtual memory (Sv39), kernel heap
- **Part 5: Traps and interrupts:** exceptions, timer, PLIC, keyboard input
- **Part 6: Engineering:** Makefile, tests, CI, versioning
- **Part 7: Debugging:** tools, reading a crash, bugs we hit and fixed
- **Part 8: What comes next:** devices, disk, processes, user mode, AI-OS
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
| Share the CPU | Chrome + Spotify at once | Timer heartbeat ready, scheduler not yet |
| Store files | C: drive, NTFS | Not yet (needs a disk driver) |
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
| 1 | `la sp, stack_top` | C code needs a stack before any function call |
| 2 | `pmpaddr0 = -1`, `pmpcfg0 = 0x1F` | **PMP** (physical memory protection) blocks S-mode from all memory by default. This entry says "S-mode may access everything" |
| 3 | `mtvec = machine_trap` | Where the CPU jumps on an M-mode trap |
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
uart_init() / plic_*()     keyboard input
echo loop                  interactive, Ctrl-D powers off
```

The order matters. For example, the heap lives at virtual address `0x90000000`, which only exists **after** paging is on, so `heap_activate()` must come after `vm_enable()`.

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
- `uart_interrupt()` reads RBR while LSR bit 0 says "data ready".

## 3.3 Logging (`kernel/logging.c`)

Built on the UART: `log_info()` prints `[INFO] ...`, `log_warn()` prints `[WARN]`, `log_trap()` prints `[TRAP]`, and `panic()` prints `[PANIC] ...` and **halts forever**. Panic is for "this should never happen, stop before making it worse".

---

# Part 4: Memory

## 4.1 Physical memory and pages

QEMU gives us **128 MiB of RAM**, from `0x80000000` to `0x88000000`.

Memory is managed in **pages** of **4 KiB (4096 bytes)**:

```text
128 MiB / 4 KiB = 32768 pages
```

**Why pages?** Tracking every byte is impossible. Tracking 32768 pages is easy, and the CPU's paging hardware works in 4 KiB pages too.

## 4.2 The bitmap page allocator (`kernel/page.c`)

One **bit per page**: 1 = used, 0 = free. 32768 pages need 32768 bits = **4 KiB** of bitmap.

```text
page_init():
  mark all pages used
  mark pages from (stack_top rounded up to 4 KiB) to RAM end as free
  → the kernel and stack pages stay reserved forever

page_alloc():   find the first 0 bit, set it to 1, return its address
page_free(p):   check the address is valid, aligned, not reserved, not already free → set bit to 0
```

**Windows comparison:** Windows' memory manager keeps a similar database of every physical page (the "PFN database").

## 4.3 Why virtual memory?

Without virtual memory, every program sees real physical addresses:
- Programs could read and overwrite each other's memory.
- Every program would need to be placed at a different address.
- Memory can't be "faked" (for example a big heap made of scattered pages).

With **virtual memory**, each address a program uses goes through a **translation table** (page table) set up by the kernel:

```text
virtual address  ──► page tables ──►  physical address
0x90000000       ──►               ──► 0x80009000 (whatever page_alloc gave)
```

**Windows comparison:** every Windows process has its own address space. Chrome's `0x10000` and Spotify's `0x10000` are different physical memory.

## 4.4 Sv39: RISC-V's paging scheme (`kernel/vm.c`)

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

### Worked example: translating `0x90000000` (the heap)

```text
VPN[2] = (0x90000000 >> 30) & 0x1FF = 2
VPN[1] = (0x90000000 >> 21) & 0x1FF = 128
VPN[0] = (0x90000000 >> 12) & 0x1FF = 0
offset =  0x90000000 & 0xFFF        = 0

root table[2]    → points to a level-1 table
level-1 table[128] → points to a level-0 table
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

**Critical lesson:** after paging is on, **any address not in the page tables causes a page fault**, including devices. That's why `vm_init()` maps the UART, the PLIC and the power device. M-mode ignores page tables, which is why the timer code running in M-mode doesn't need a mapping.

## 4.5 The kernel heap (`kernel/heap.c`)

Pages are 4 KiB, but the kernel often needs 24 bytes or 3000 bytes. The **heap** splits pages into variable-size blocks: that's `kmalloc(size)` / `kfree(ptr)`.

```text
0x90000000
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
| **Growth** | If nothing fits, `page_alloc()` a new page and map it at the heap end | The heap grows as needed, up to `0xA0000000` |
| **Alignment** | Sizes are rounded up to 8 bytes | `uint64_t` values must be 8-byte aligned |
| **Safety** | `kfree` ignores NULL, unknown pointers and double frees | Bugs shouldn't corrupt the heap |

**Windows comparison:** the Windows kernel's "pool allocator" (`ExAllocatePool`) does this job.

---

# Part 5: Traps and Interrupts

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
 → uart_interrupt(): read every byte into the ring buffer
 → plic_complete(10)
 → sret: back to the echo loop
 → uart_getc() finds 'a' in the buffer → uart_putc('a')
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

QEMU `virt` has a "test device" at `0x00100000`. Writing `0x5555` exits QEMU (power off), and `0x7777` reboots. **Ctrl-D** in the echo loop calls `power_off()`. It powers off only the KnocOS virtual machine, not your real computer.

---

# Part 6: Engineering

## 6.1 The Makefile

- **Pattern rules** (`%.o: %.c`) compile any C file the same way.
- **`-MMD -MP`**: the compiler writes a `.d` file listing every header each `.c` file includes, and `make` reads them. Changing a header rebuilds exactly the files that use it.
- **`-Wall -Wextra -Werror`**: turns on many warnings and makes **any warning a build error**. Warnings often hide real bugs.
- **`-DKNOCOS_VERSION='"v0.4.0"'`**: the version from the `VERSION` file becomes a C string the kernel prints at boot.

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

To release: update `VERSION` and `CHANGELOG.md`, commit, then `git tag v0.4.0 && git push --tags`.

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

---

# Part 8: What Comes Next

## 8.1 Device abstraction (finishes Phase 4)

One common shape for every driver:

```c
struct device {
    const char *name;
    uint32_t irq;
    void (*init)(void);
    void (*interrupt)(void);
    ...
};
```

Drivers register themselves, and the trap handler finds the owner of an IRQ instead of hard-coding "IRQ 10 = UART". **Windows comparison:** the Windows driver model, where every driver has the same entry points.

## 8.2 virtio-blk: a disk

A virtual disk backed by a file (`disk.img`) on your PC, like a VirtualBox `.vdi`. The driver shares a **virtqueue** (a ring buffer of requests) with the device:

```text
request = [header: READ sector N] → [data buffer] → [status byte]
kernel writes it to the virtqueue → notifies the device → device does the work
→ interrupt (through the PLIC) → kernel checks the status → data is ready
```

This is needed for the filesystem, and later for storing AI models.

## 8.3 Processes and the scheduler (Phase 5)

- **Process:** a running program with its own registers, stack and (later) page table.
- **Context switch:** save process A's registers and load process B's. It's the same idea as the trap frame.
- **Scheduler:** on every timer tick (`timer_tick()`), decide who runs next. Start with **round-robin** (take turns).

## 8.4 User mode and system calls

- Programs run in **U-mode** with `U` bit page mappings, so they can't touch the kernel or devices.
- To ask the kernel for something, a program runs **`ecall`**, which traps to S-mode (exception 8). The kernel reads the request number in `a7` and the arguments in `a0`–`a5`.
- **Windows comparison:** `syscall` into `ntoskrnl`, like `NtReadFile` and `NtCreateFile`.

## 8.5 Toward the AI-OS

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
| `0x10001000` | virtio devices (IRQ 1–8) | Not yet |
| `0x80000000` | RAM start, kernel | ✅ identity, 128 MiB |
| `0x88000000` | RAM end | |
| `0x90000000` | Kernel heap (virtual) | ✅ grows on demand |

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
riscv64-unknown-elf-objdump -d knocos.elf | less    # disassembly
riscv64-unknown-elf-nm -n knocos.elf                # symbols
```

## Glossary

| Term | Meaning |
|---|---|
| **Bare metal** | Running with no OS underneath |
| **CLINT** | Core-local interruptor: the timer (and software interrupts) hardware |
| **Coalescing** | Merging neighbouring free heap blocks |
| **Context switch** | Saving one program's registers and loading another's |
| **CSR** | Control and status register: CPU configuration |
| **ELF** | Executable file format used on Linux and bare-metal RISC-V |
| **Hart** | Hardware thread: a RISC-V CPU core |
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
| **virtio** | Standard simple interface for virtual devices |
| **volatile** | C keyword: "this memory can change behind the compiler's back" |
| **wfi** | Wait for interrupt: sleep until something happens |
