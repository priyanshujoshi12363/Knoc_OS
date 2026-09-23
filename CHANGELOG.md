# Changelog

All notable changes to KnocOS are listed here. Versions follow [Semantic Versioning](https://semver.org/): while KnocOS is below `1.0.0`, every minor version is a development milestone.

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
