
# KnocOS

[![CI](https://github.com/priyanshujoshi12363/Knoc_OS/actions/workflows/ci.yml/badge.svg)](https://github.com/priyanshujoshi12363/Knoc_OS/actions/workflows/ci.yml)
![Version](https://img.shields.io/badge/version-v0.6.0-blue)
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
- **Automated tests** (`make test`) and **GitHub Actions CI** on every push
- Make-based build with automatic header dependencies and `-Wall -Wextra -Werror`

### Boot output

```text
[INFO] KnocOS v0.6.0 starting
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
[INFO] Device ready: uart0 (IRQ 10)
[INFO] Device ready: power0
[INFO] Device ready: disk0 (IRQ 1)
[INFO] Devices: 3
       uart0  IRQ 10  ready
       power0  ready
       disk0  IRQ 1  2048 blocks  ready
[INFO] Device table verified
[INFO] Disk sectors: 2048
[INFO] Disk write/read test passed
[INFO] Disk says: Hello from the host!     ← text written into disk.img from Linux
[INFO] Disk boot count: 1                  ← stored on the disk, +1 on every boot
[INFO] All self-tests passed
[INFO] Keyboard echo ready, start typing (Ctrl-D to power off)
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

## ✅ Just Completed: virtio-blk Disk Driver

Phase 4 (interrupts, traps and devices) is complete, and KnocOS now has **permanent storage**: a virtio-blk disk driver built on the device abstraction. **Next up: processes and the scheduler (Phase 5)** or the **filesystem (Phase 6)**.

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
| *(uncommitted)* | virtio-blk disk driver (`disk0`), block devices in the device model, version `v0.6.0` |

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
- Self-tests: the disk test writes and reads back a sector, reads text placed in `disk.img` by the host, and increments a boot counter stored on the disk
- Self-tests: the timer test waits for 5 kernel ticks (1 s timeout, and they must not arrive faster than 10 ms apart), and the trap test runs `ebreak` and checks the handler ran and returned

Next steps:

- [ ] Processes, context switching and a timer-driven scheduler (Phase 5)
- [ ] A simple filesystem on `disk0` (Phase 6)

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
   └─ keyboard echo loop (wfi when idle)

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
│   └── virtio_blk.c/h# virtio-blk disk driver (disk0)
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

- Sets `sp` to `stack_top`
- Configures PMP entry 0 as NAPOT covering all memory with `RWX`, which Supervisor mode needs in order to run
- Installs `machine_trap` (`mtvec`) and `supervisor_trap` (`stvec`)
- Delegates exceptions to S-mode (`medeleg = 0xB1FF`) and the supervisor software + external interrupts (`mideleg = SIP_SSIP | SIP_SEIP`)
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
- Every other trap prints `scause`, `sepc`, `stval`, `ra` and `sp`, then panics
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

Adding a new device means writing its driver file and calling its register function. The trap handler and the PLIC setup don't change, and the disk driver proved it.

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
-DKNOCOS_VERSION='"v0.6.0"'  # from the VERSION file
```

Header files and `boot/linker.ld` are tracked automatically, so `make` always rebuilds what changed.

### Testing

`make test` runs `scripts/test.sh`, which:

1. Creates a fresh temporary disk image with `Hello from the host!` in sector 0
2. **Boot 1:** checks that every self-test message appears (including the disk tests and `Disk boot count: 1`) and there is no `[PANIC]`
3. Types `knocos-echo-test` + Enter and checks the echo (this tests the UART → PLIC → trap path)
4. Presses Ctrl-D and checks that KnocOS powers QEMU off within 15 seconds
5. **Boot 2** with the same disk image: checks `Disk boot count: 2`, which proves data written to the disk survives a reboot

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

- [ ] Process structure
- [ ] Context switching
- [ ] Scheduler (timer-driven preemption)
- [ ] User mode
- [ ] System calls
- [ ] Multiple processes

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
| Processes, scheduler, context switching | ⬜ |
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
| Crash recovery (no single fault takes down the whole OS) | ⬜ |
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

**Early development (v0.6.0):** being built toward a production-grade OS. Not yet ready for real-world use.

Boot, logging, physical memory, Sv39 paging and the kernel heap are working. Timer interrupts (forwarded to the kernel) and Supervisor-mode exception handling are working. The PLIC and an interrupt-driven UART driver are in, so KnocOS now reacts to the keyboard. A power-off driver, automated tests and CI are in place. Device abstraction is done, so **Phase 4 is complete**, and a virtio-blk disk driver gives KnocOS permanent storage.
