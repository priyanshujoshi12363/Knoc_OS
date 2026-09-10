
# KnocOS

**KnocOS** is a from-scratch experimental operating system being built for learning and exploring low-level computer architecture, operating systems, memory management, and eventually AI-oriented systems.

The project starts with a minimal RISC-V kernel running on QEMU and will gradually evolve toward a more complete operating system.

> **Learn → Build → Inspect → Debug → Understand → Repeat**

---

## Current Status

KnocOS currently has:

- RISC-V 64-bit bare-metal boot code
- Custom linker script
- Kernel entry point
- C kernel support
- Stack initialization
- UART output through memory-mapped I/O
- Basic memory layout information
- Kernel and stack boundary symbols
- Basic memory accounting functions
- QEMU `virt` machine support
- Make-based build system
- ELF kernel generation

The kernel currently runs at:

`text
0x80000000

on the QEMU RISC-V `virt` machine.


## Architecture

KnocOS currently targets:

text
Architecture: RISC-V 64-bit
ISA:          RV64G
ABI:          LP64D
Machine:      QEMU virt
Kernel base:  0x80000000
RAM:          128 MiB

The current QEMU memory layout provides:

```text
0x80000000 ─────────────── RAM START
     │
     │  KnocOS Kernel
     │
0x8000007c ─────────────── Kernel End
     │
     │  Reserved Kernel Stack
     │
0x80004080 ─────────────── Stack Top
     │
     │  Free RAM
     │
0x88000000 ─────────────── RAM END
```

---

## Project Structure

```text
KnocOS/
│
├── boot/
│   ├── boot.S
│   └── linker.ld
│
├── kernel/
│   ├── main.c
│   └── memory.c
│
├── Makefile
├── README.md
└── .gitignore
```

### `boot/boot.S`

Contains the earliest kernel startup code.

Responsibilities currently include:

* Setting the stack pointer
* Calling the C kernel entry point
* Keeping the CPU alive after kernel execution

---

### `boot/linker.ld`

Controls the layout of the kernel inside memory.

It defines:

* Kernel start
* `.text`
* `.rodata`
* `.data`
* `.bss`
* Kernel end
* Kernel stack region
* Stack boundaries

---

### `kernel/main.c`

Contains the main C kernel logic.

Current responsibilities include:

* UART output
* Printing kernel information
* Calling memory-management functionality

---

### `kernel/memory.c`

Contains the initial memory-management layer.

Current functions provide information about:

* Total RAM
* Total RAM in KB/MB/GB
* Kernel size
* Stack size
* Used memory
* Free memory

Actual page allocation and dynamic memory management will be implemented later.

---

## Building

Make sure the RISC-V cross compiler is installed:

```bash
riscv64-unknown-elf-gcc
```

Then build the kernel:

```bash
make
```

This produces:

```text
knocos.elf
```

---

## Running

KnocOS can be launched using QEMU:

```bash
make run
```

The project uses:

```text
QEMU RISC-V virt machine
BIOS disabled
ELF loaded directly as kernel
Serial output through UART
```

---

## Toolchain

The project currently uses:

* GCC RISC-V cross compiler
* GNU assembler
* GNU linker
* QEMU
* GDB
* `objdump`
* `readelf`
* `nm`
* Device Tree Compiler
* Git

---

## Debugging

KnocOS can be inspected using GDB and QEMU.

QEMU can be started paused with a GDB server:

```bash
env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin \
qemu-system-riscv64 \
-machine virt \
-bios none \
-kernel knocos.elf \
-nographic \
-S \
-gdb tcp::1234
```

Then connect using:

```bash
gdb-multiarch knocos.elf
```

Inside GDB:

```gdb
target remote :1234
```

Useful commands include:

```gdb
info registers
info registers pc
info registers sp
stepi
```

---

## Inspecting the ELF

View sections:

```bash
riscv64-unknown-elf-readelf -S knocos.elf
```

View symbols:

```bash
riscv64-unknown-elf-nm -n knocos.elf
```

Disassemble the kernel:

```bash
riscv64-unknown-elf-objdump -d knocos.elf
```

---

## Memory Model

The current KnocOS memory model is intentionally simple.

QEMU provides:

```text
128 MiB RAM
```

starting at:

```text
0x80000000
```

and ending at:

```text
0x88000000
```

KnocOS currently reserves space for:

1. The kernel
2. The kernel stack

The remaining region is considered free memory.

This is an early educational memory model and is **not yet a complete physical memory manager**.

---

## Roadmap

### Phase 1 — Boot

* [x] RISC-V kernel entry
* [x] Linker script
* [x] Stack initialization
* [x] Boot into QEMU
* [x] C kernel entry

### Phase 2 — Kernel Basics

* [x] UART output
* [x] Basic kernel structure
* [x] Memory layout symbols
* [x] Basic memory information
* [ ] Better kernel logging
* [ ] Panic handler

### Phase 3 — Memory Management

* [x] Detect/basic RAM model
* [x] Kernel memory boundaries
* [x] Stack boundaries
* [x] Used/free memory calculation
* [ ] Page alignment
* [ ] Physical page allocator
* [ ] Page tracking
* [ ] Virtual memory
* [ ] RISC-V Sv39 page tables
* [ ] Kernel heap
* [ ] `kmalloc` / `kfree`

### Phase 4 — Hardware & Interrupts

* [ ] UART driver
* [ ] Timer
* [ ] Interrupt controller
* [ ] Trap handling
* [ ] Interrupt handling
* [ ] Device abstraction

### Phase 5 — Processes & Scheduling

* [ ] Process structure
* [ ] Context switching
* [ ] Scheduler
* [ ] User mode
* [ ] System calls
* [ ] Multiple processes

### Phase 6 — Storage & Filesystems

* [ ] Block device
* [ ] Disk driver
* [ ] Filesystem
* [ ] File descriptors
* [ ] VFS layer

### Phase 7 — User Space

* [ ] User programs
* [ ] Shell
* [ ] Standard library
* [ ] Process management

### Future Direction

Eventually, KnocOS is intended to explore:

* AI-native operating-system concepts
* Local AI runtimes
* Hardware acceleration
* AI-specific scheduling
* AI-aware memory management
* Custom RISC-V hardware
* FPGA-based CPU experimentation
* Custom AI accelerators

These are long-term goals and are not part of the current implementation.

---

## Philosophy

KnocOS is being built from the lowest practical level upward.

Instead of starting with a large framework, the project focuses on understanding what is happening underneath:

```text
Hardware
   ↓
RISC-V ISA
   ↓
Boot code
   ↓
Linker
   ↓
Kernel
   ↓
Memory management
   ↓
Interrupts
   ↓
Processes
   ↓
System calls
   ↓
Filesystem
   ↓
User space
   ↓
Applications
```

The goal is not simply to produce an operating system.

The goal is to understand **why every layer exists and how the layers communicate with each other.**

---

## Status

**Early development — experimental / educational**

KnocOS is currently a learning-focused bare-metal RISC-V kernel running on QEMU.

More functionality will be added incrementally as the architecture and kernel evolve.


