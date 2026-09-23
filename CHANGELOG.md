# Changelog

All notable changes to KnocOS are listed here. Versions follow [Semantic Versioning](https://semver.org/): while KnocOS is below `1.0.0`, every minor version is a development milestone.

## [0.12.0] - 2026-09-23

Wait queues and the KnocFS filesystem: programs and AI models live on disk.

### Added
- Wait queues: `process_block(channel, timeout)` / `process_wake(channel)`, a `BLOCKED` state, and `scheduler_preempt()` after device interrupts so an interactive process runs as soon as it's woken
- Sleep locks (`sleeplock_acquire` / `release`); a process that crashes has its sleep locks released
- The console and the `read` system call sleep until a key arrives (the UART interrupt wakes them); `process_wait()` sleeps until the process exits; the guardian is woken at once by a crash
- Disk: requests of up to 8 sectors (`read_blocks` / `write_blocks`, `device_read_blocks` / `device_write_blocks`), a sleep lock, and the requesting process sleeps until the completion interrupt
- KnocFS (`kernel/knocfs.c`): superblock, free-block bitmap, 1024 inodes with up to 12 extents each, directories, absolute paths; `mount`, `lookup`, `create`, `remove`, `stat`, `readdir`, `truncate`, `read`, `write`, `usage`
- File system calls: `open`, `close`, `seek`, `stat`, `readdir`, `mkdir`, `remove`; `read`/`write` take a file descriptor (0 keyboard, 1–2 screen, 3+ files); capabilities `FILES_READ` / `FILES_WRITE`; 8 open files per process; file I/O goes directly into the program's pages
- Programs load from `/bin/<name>` on disk, with the built-in copies as a fallback
- Host tool `tools/knocfs.py` (format, mkdir, put, put-text, ls, cat, rm, info, make-test-model) and `scripts/mkdisk.sh`; `make put`, `make ls`, `make sync-programs` (run by `make run`)
- Programs `files` (a note that survives reboots, `/bin` listing, 20 KB write/read/remove) and `modelcheck` (AI_AGENT, loads an 8 MiB model file and checks every byte)
- Self-tests: 200 disk reads while another process runs, a multi-sector read equal to 8 single reads, programs loaded from disk
- `make test`: the Boot 1 note is found on Boot 2

### Changed
- `disk.img` is 64 MiB (`DISK_MB`) with a KnocFS filesystem; `make reset-disk` builds it with `scripts/mkdisk.sh`
- `process_restart()` reloads a program outside the interrupts-off region (it may read from disk)
- `noperm` also checks that `open` is refused without a FILES capability

### Fixed
- Program data segments could start inside the last code page; the loader then mapped a zeroed page over the code. User programs now page-align their data segment, and `elf_load()` rejects segments that share a page
- Two processes using the disk at the same time could mix up their requests (shared descriptors and buffer); the disk now has a sleep lock
- `make reset-disk` with a bad size (for example `DISK_MB=4096.`) deleted `disk.img` before failing; `mkdisk.sh` now checks the size first

## [0.11.0] - 2026-09-23

User mode and system calls: programs can't touch the kernel.

### Added
- User programs run in U-mode with their own page table (`vm_user_create`): the kernel's root entries are shared without the U bit, and the user slots (`0x1000000000`–`0x1FFFFFFFFF`) are private. The scheduler switches `satp` on every process switch
- Trap entry from U-mode: `sscratch` holds the process's kernel stack while a program runs (0 in the kernel); `supervisor_trap` swaps it with `sp` and returns with `sret` to the right mode. `user_enter()` starts a program with all registers cleared
- 9 system calls (`kernel/syscall_abi.h`): `exit`, `write`, `read`, `getpid`, `yield`, `sleep`, `uptime`, `spawn`, `mem_alloc`, with error codes
- Safe user copies: every user pointer is translated in the program's own page table (user page + right permission required), so bad pointers return `E_FAULT`
- Capabilities per program (`CONSOLE`, `SPAWN`, `MEMORY`): denied calls are refused and logged `[SECURITY]`
- Memory quotas: 16 MiB for normal programs, 1 GiB for `AI_AGENT` programs; blocks are zeroed and mapped with megapages when large
- System call trace: the last 8 calls and the number of denied calls per process go to the AI space with a crash report. The AI prints them, diagnoses user faults (null pointer, own memory, outside its space) and does not restart a program that made forbidden calls (the kernel's fallback rule does the same)
- ELF loader (`kernel/elf.c`) and built-in programs embedded with `.incbin` (`kernel/programs.S`, `kernel/program.c`)
- User library (`user/crt0.S`, `user/ulib.c`) and 7 programs: `hello`, `badcall`, `noperm`, `hog`, `bigmem`, `crash`, `spy`
- Self-test: 5 programs must exit with 0, and every page must be returned afterwards (no leaks)
- Test keys: Ctrl-U (`crash` program), Ctrl-E (`spy` program)
- `process_spawn`, `process_wait`, `process_exit_code`; a `LOADING` state so a program isn't scheduled before it's loaded
- `make test`: user program checks, and a failure if `spy` ever reads kernel memory

### Changed
- `memset` writes 8 bytes at a time (large program blocks are cleared quickly)
- `vm.c` mapping functions take a root table, so the same code maps the kernel and user programs
- `boot.S` clears `sscratch` at boot

## [0.10.0] - 2026-09-23

Big memory: room for AI models.

### Added
- Device tree parser (`kernel/fdt.c`): RAM start/size from `/memory` and the CPU count from `/cpus`. QEMU passes the device tree address in `a1`; `boot.S` hands it to `kernel_main(dtb)` and `aispace_main(dtb)`
- Buddy page allocator (`kernel/page.c`): blocks of 4 KiB up to 1 GiB (orders 0–18), splitting on allocation and merging buddies on free, one metadata byte per page. New `page_alloc_order()`, `page_alloc_contiguous()`, `page_largest_free()`, `page_ram_start()` / `page_ram_end()`
- 2 MiB megapages: `vm_map_range()` maps with level-1 leaf entries when aligned (2 GiB of RAM = 1026 entries). `vm_map()` refuses to map inside a megapage, and `vm_debug()` understands them
- Spinlocks (`kernel/spinlock.c`): `spin_lock` / `spin_unlock` (interrupts off while held), `spin_trylock`, used by the heap and the page allocator
- Shared UART lock between the kernel and the AI space: a core owns the UART for a whole line (`console_owner` in the mailbox, compare-and-swap, 10 ms timeout)
- Self-tests: spinlock, a 64 MiB aligned contiguous block, 1024 single pages freed and merged back, the largest free block (1 GiB) written and read through megapages
- `make test` checks the RAM size, CPU count, megapage count and that kernel and AI space output never mix on one line

### Changed
- QEMU runs with 2 GiB (`-m 2G`); KnocOS needs at least 1 GiB and works with 8 GiB+
- AI space: 256 MiB at `0x90000000` (was 16 MiB at `0x87000000`)
- Kernel heap virtual region moved to `0x2000000000` (`0x90000000` is now real RAM)
- On a warm restart the AI space restores the device tree address in `a1` before jumping to `_start`
- The AI's "bad pointer" diagnosis uses the real RAM end (published by the kernel in the mailbox)
- `memory.c` helpers and `make pages` / `make size` use the real memory layout instead of 128 MiB

## [0.9.0] - 2026-09-23

Fault containment and warm kernel restart: the AI never stops.

### Added
- Fault containment: an exception in a process that ran with interrupts on stops only that process (`[OOPS]`, state `CRASHED`), like a Linux oops. Kernel code, interrupt handlers and code with interrupts off still count as a kernel crash
- AI verdicts for process crashes: the kernel's `guardian` process posts each crashed process to the mailbox (`fault_seq`), the AI space diagnoses it and replies (`verdict_seq`) with `RESTART_PROCESS` and/or `DISABLE_DRIVER`. After 3 restarts a process is left stopped. Without the AI space the kernel applies the same rules after a 1 s timeout
- `process_crash`, `process_next_crash`, `process_restart` (in place, same stack, new pid) and `process_discard`
- Driver tracking: every call into a driver (init, interrupt, read, write, block I/O) records the active driver per process, so crash reports say which driver crashed
- `device_disable()` and `plic_disable()`. Drivers on the AI space's disabled list are skipped at boot (`Device disabled by the AI space`)
- `faulty0`: a test driver with a bug on purpose (`kernel/faulty.c`)
- Warm kernel restart: the AI space copies the clean kernel image (`kernel_start`..`kernel_image_end`, 38 KiB) into its protected memory at boot. On a crash or freeze it stops core 0 with a machine software interrupt, parks it in AI space code (`aispace_park_core0`), saves the black box, restores the clean kernel and restarts only core 0 at `_start`
- Boot handshake (`boot_request` / `boot_ack`): core 0 waits in `boot.S` until the AI space has taken its clean copy (1 s timeout when there is no AI space)
- Kernel code check: before restoring, the AI space compares the running kernel code with the clean copy and reports corrupted bytes in its diagnosis
- New black box actions: warm restart, warm restart into safe mode. Records now include the driver name
- Halting after 4 crashes in a row keeps core 0 stopped while the AI space stays online
- Test keys: Ctrl-X (driver fault), Ctrl-K (kernel fault with interrupts off), Ctrl-O (overwrite kernel code)
- `make test` run 3: containment, driver disabling, freeze / code corruption / kernel fault → warm restarts #1–#3 → safe mode, with no machine reboot. Run 4: the 4th crash halts the kernel and the AI stays online

### Changed
- Ctrl-F now crashes only the console process (contained) instead of the kernel
- Crash recovery uses a warm kernel restart; a full machine reboot is only the fallback when core 0 can't be stopped
- `panic()` turns interrupts off first, so no other process runs after a panic
- The `guardian` process runs for the whole uptime (fault handling + the 60 s crash-streak reset)
- `plic_init()` clears all IRQ enables, and `plic_enable()` completes any interrupt a crashed kernel left claimed
- `boot.S` clears `sstatus.SIE` at boot and enables `mie.MSIE` on core 0
- Diagnosis texts are neutral ("the code accessed ...") since they now describe processes too

## [0.8.0] - 2026-09-23

The AI space: AI that survives kernel crashes.

### Added
- AI space on CPU core 1 (`kernel/aispace.c`): runs in M-mode in its own 16 MiB region at `0x87000000`, self-contained (own UART output, clock, polled virtio-blk driver), linked into its own sections
- PMP protection: the kernel (core 0, S-mode) cannot read, write or execute the AI space. A boot self-test probes it
- Kernel ↔ AI space mailbox (`kernel/mailbox.h`): heartbeat, uptime, current process, crash details, and `last_kernel_pc` written by the M-mode timer
- Crash detection (panic and unhandled traps) and freeze detection (no heartbeat for 2 s)
- Rule-based diagnosis (Tier 0 brain): null pointer, bad pointer, page fault, protected memory, illegal instruction, misaligned access, panic, infinite loop
- Black box (`kernel/blackbox.h`): crash reports in the last 8 sectors of the disk, written by the AI space; the kernel prints new reports at boot (`kernel/guardian.c`)
- Recovery actions: reboot, safe mode after 3 crashes in a row, halt after 4 (boot-loop protection); a `guardian` process resets the streak after 60 s without a crash
- Test keys: Ctrl-F (bad pointer), Ctrl-P (panic), Ctrl-W (freeze)
- `trap_probe_read()`: a read that returns an error instead of crashing on a bad address
- `make test` boots 3–6: fault → freeze → panic → safe mode, all detected, diagnosed, saved and recovered
- Milestone roadmap v0.8 → v1.0 in `goal.md` and README

### Changed
- QEMU runs with 2 cores (`-smp 2`)
- `boot.S`: routes core 1 to the AI space, clears `.bss`, sets 2 PMP entries, resets `satp` and interrupt state on every boot
- Linker script: AI space sections, explicit small-data sections, `bss_start`/`bss_end`, a `.mailbox` section
- The page allocator stops below the AI space
- `panic()` and unhandled traps report to the AI space

### Fixed
- `.bss` was never cleared: it only worked because the first boot started from zeroed RAM. A warm reboot kept old values
- After a warm reboot, paging (`satp`) was still on from the previous boot, so the new kernel corrupted its own live page tables

## [0.7.0] - 2026-09-23

Processes and the AI-aware scheduler.

### Added
- Processes (kernel threads) with their own 16 KiB stacks, saved context, class, state and CPU accounting (`kernel/process.c/h`)
- `context_switch` in assembly (`kernel/switch.S`)
- Timer-driven preemption: `scheduler_tick()` on every tick
- AI-aware scheduler: INTERACTIVE first, then weighted virtual-runtime sharing between AI_AGENT (60), NORMAL (30) and BACKGROUND (10), with per-class time slices and no starvation
- `process_create`, `process_yield`, `process_sleep`, `process_exit`, `process_kill`, `process_list`
- Scheduler self-test: three CPU-bound workers must get 60/30/10 (±10) of the CPU, all must progress, and an interactive process must wake on its target tick
- The keyboard echo runs as an INTERACTIVE `console` process
- `memcpy` / `memset` (`kernel/string.c`), required by the compiler for struct copies

### Changed
- M-mode uses its own private stack through `mscratch`, so the timer handler never touches process stacks
- The S-mode trap handler saves and restores `sepc` and `sstatus`, so switching processes inside a trap is safe
- `kernel_main` becomes the idle process (pid 0) after boot

## [0.6.0] - 2026-09-23

Disk driver: KnocOS has permanent storage.

### Added
- virtio-blk disk driver (`kernel/virtio_blk.c`), registered as block device `disk0` (IRQ 1): feature negotiation, an 8-entry virtqueue, interrupt-driven 512-byte sector reads and writes
- virtio-mmio definitions (`kernel/virtio.h`)
- Block devices in the device model: `block_size`, `block_count`, `read_block`, `write_block`, and `device_read_block` / `device_write_block` with range checks
- `disk.img` (1 MiB) created by the Makefile and attached by `make run`, plus `make reset-disk`
- Disk self-test: write/read-back of a sector, reading text placed by the host, and a boot counter stored on the disk
- `make test` boots twice on a fresh test disk and checks the boot counter goes from 1 to 2 (data survives reboots)
- `log_info_uint` and `log_info_text` logging helpers

### Changed
- The virtio slot (`0x10001000`) is mapped in the kernel page tables
- `device_list` shows the block count of block devices
- No changes were needed in `trap.c` or `plic.c` to add the disk

## [0.5.0] - 2026-09-23

Device abstraction. Phase 4 (hardware and interrupts) is complete.

### Added
- Device abstraction (`kernel/device.c/h`): a common `device_t` driver shape (`init`, `interrupt`, `read`, `write`) and a device table
- `device_register`, `device_init_all`, `device_find`, `device_read`, `device_write`, `device_handle_irq`, `device_list`
- UART registered as device `uart0` (IRQ 10), power device as `power0`
- Boot log lists every device, and a self-test checks the device table
- `make test` checks that `uart0` and `power0` are ready

### Changed
- The trap handler dispatches device interrupts through the device table instead of hard-coding the UART IRQ
- `kernel_main` starts devices with `device_init_all()`, and the echo loop uses `device_read` / `device_write`
- UART internals (`init`, `interrupt`, `getc`) are private to the driver, and only the early-console output functions stay public

## [0.4.0] - 2026-09-23

Hardware interrupts and traps.

### Added
- Supervisor-mode trap handler in C with exception decoding (`scause`, `sepc`, `stval`)
- Exceptions delegated to S-mode (`medeleg`)
- Timer interrupts forwarded from M-mode to the kernel (`mip.SSIP` + `mideleg`)
- PLIC interrupt controller driver (`plic_enable`, `plic_claim`, `plic_complete`)
- Interrupt-driven UART input with a 128-byte ring buffer and keyboard echo
- Power-off / reboot driver (QEMU test device); Ctrl-D powers off
- `make test`: automated boot test that checks every self-test and powers off
- GitHub Actions CI: build + `make test` on every push
- Version number shown at boot (`VERSION` file)

### Changed
- Timer read with `rdtime`, first deadline armed in M-mode, drift-free re-arming
- Timer constants shared between C and assembly (`timer.h`)
- One UART driver (`uart.c`) replaces three copies of UART code
- Makefile: automatic header dependencies (`-MMD -MP`), `-Wall -Wextra -Werror`
- Linker script: separate `R-X` and `RW-` segments

### Fixed
- Timer self-test failing because it depended on CPU speed

## [0.3.0] - 2026-09-13

Kernel heap.

### Added
- `kmalloc` / `kfree` in a dedicated virtual region (`0x90000000`)
- Block splitting, coalescing and automatic page growth
- Heap stress test

## [0.2.0] - 2026-09-13

Physical and virtual memory.

### Added
- Bitmap physical page allocator (`page_alloc` / `page_free`)
- Sv39 page tables, identity-mapped kernel, paging enabled

## [0.1.0] - 2026-09-10

First boot.

### Added
- RISC-V boot code, linker script, stack, C kernel entry
- UART output and basic memory accounting
- Make-based build and QEMU `virt` support
