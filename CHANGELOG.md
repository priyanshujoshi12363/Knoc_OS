# Changelog

All notable changes to KnocOS are listed here. Versions follow [Semantic Versioning](https://semver.org/): while KnocOS is below `1.0.0`, every minor version is a development milestone.

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
