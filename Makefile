CC = riscv64-unknown-elf-gcc
AS = riscv64-unknown-elf-gcc
LD = riscv64-unknown-elf-ld

VERSION := $(shell cat VERSION)

CFLAGS = -march=rv64g -mabi=lp64d -mcmodel=medany \
         -ffreestanding -fno-pie -fno-pic \
         -nostdlib -nostartfiles -nodefaultlibs \
         -Wall -Wextra -Werror \
         -MMD -MP \
         -DKNOCOS_VERSION='"v$(VERSION)"'

QEMU = env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin qemu-system-riscv64
RAM ?= 2G
QEMU_FLAGS = -machine virt -smp 2 -m $(RAM) -bios none -nographic

DISK = disk.img
DISK_SECTORS = 2048
DISK_MESSAGE = Hello from the host!
QEMU_DISK_FLAGS = -global virtio-mmio.force-legacy=false \
                  -drive file=$(DISK),if=none,format=raw,id=disk0 \
                  -device virtio-blk-device,drive=disk0,bus=virtio-mmio-bus.0

KERNEL_OBJS = boot/boot.o \
              kernel/main.o \
              kernel/memory.o \
              kernel/logging.o \
              kernel/page.o \
              kernel/vm.o \
              kernel/heap.o \
              kernel/timer.o \
              kernel/trap.o \
              kernel/uart.o \
              kernel/plic.o \
              kernel/power.o \
              kernel/device.o \
              kernel/virtio_blk.o \
              kernel/process.o \
              kernel/switch.o \
              kernel/string.o \
              kernel/guardian.o \
              kernel/faulty.o \
              kernel/fdt.o \
              kernel/spinlock.o \
              kernel/syscall.o \
              kernel/elf.o \
              kernel/program.o \
              kernel/programs.o \
              kernel/aispace.o

TIMER_OBJS = timer/timer.o

USER_PROGRAMS = hello badcall noperm hog bigmem crash spy
USER_LIB_OBJS = user/crt0.o user/ulib.o
USER_ELFS = $(USER_PROGRAMS:%=user/%.elf)
USER_OBJS = $(USER_LIB_OBJS) $(USER_PROGRAMS:%=user/%.o)

DEPS = $(KERNEL_OBJS:.o=.d) $(TIMER_OBJS:.o=.d) $(USER_OBJS:.o=.d)

.PHONY: all clean run test timer-test size pages reset-disk

all: knocos.elf

knocos.elf: $(KERNEL_OBJS) boot/linker.ld
	$(LD) -T boot/linker.ld -o knocos.elf $(KERNEL_OBJS)

user/%.elf: user/%.o $(USER_LIB_OBJS) user/linker.ld
	$(LD) -T user/linker.ld -s -o $@ $(USER_LIB_OBJS) $<

kernel/programs.o: $(USER_ELFS)

timer.elf: $(TIMER_OBJS) timer/linker.ld
	$(LD) -T timer/linker.ld -o timer.elf $(TIMER_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(AS) $(CFLAGS) -c -o $@ $<

kernel/main.o: VERSION

-include $(DEPS)

clean:
	rm -f knocos.elf timer.elf $(KERNEL_OBJS) $(TIMER_OBJS) $(USER_OBJS) $(USER_ELFS) $(DEPS)

$(DISK):
	dd if=/dev/zero of=$(DISK) bs=512 count=$(DISK_SECTORS) status=none
	printf '$(DISK_MESSAGE)' | dd of=$(DISK) conv=notrunc status=none

reset-disk:
	rm -f $(DISK)
	$(MAKE) $(DISK)

run: knocos.elf $(DISK)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_DISK_FLAGS) -kernel knocos.elf

test: knocos.elf
	./scripts/test.sh

timer-test: timer.elf
	$(QEMU) $(QEMU_FLAGS) -kernel timer.elf

size: knocos.elf
	@echo "================================"
	@echo "        KnocOS Size Info"
	@echo "================================"
	@echo ""
	@riscv64-unknown-elf-size knocos.elf
	@echo ""
	@echo "Kernel:"
	@python3 -c "import subprocess; s=subprocess.check_output(['riscv64-unknown-elf-nm','-n','knocos.elf'],text=True); d={line.split()[-1]:int(line.split()[0],16) for line in s.splitlines() if len(line.split())>=3}; print(f'  Size: {(d[\"kernel_end\"]-d[\"kernel_start\"])/1024:.2f} KiB')"
	@echo ""
	@echo "Stack:"
	@python3 -c "import subprocess; s=subprocess.check_output(['riscv64-unknown-elf-nm','-n','knocos.elf'],text=True); d={line.split()[-1]:int(line.split()[0],16) for line in s.splitlines() if len(line.split())>=3}; print(f'  Size: {(d[\"stack_top\"]-d[\"stack_bottom\"])/1024:.2f} KiB')"
	@echo ""
	@echo "RAM:"
	@echo "  make run gives QEMU: $(RAM) (change with RAM=8G)"
	@echo "  The kernel reads the real size from the device tree at boot"

pages: knocos.elf
	@echo "================================"
	@echo "        KnocOS Memory Layout"
	@echo "================================"
	@echo ""
	@echo "Physical Memory:"
	@echo "  RAM size    : $(RAM) with make run (the kernel reads the real size at boot)"
	@echo "  RAM Start   : 0x80000000"
	@echo "  AI space    : 0x90000000 - 0x9FFFFFFF (256 MiB, PMP protected)"
	@echo "  Page size   : 4 KiB, buddy blocks up to 1 GiB, 2 MiB megapages"
	@echo ""
	@echo "Kernel:"
	@python3 -c "import subprocess; s=subprocess.check_output(['riscv64-unknown-elf-nm','-n','knocos.elf'],text=True); d={line.split()[-1]:int(line.split()[0],16) for line in s.splitlines() if len(line.split())>=3}; print(f'  Image       : 0x{d[\"kernel_start\"]:08X} - 0x{d[\"kernel_image_end\"]:08X} ({(d[\"kernel_image_end\"]-d[\"kernel_start\"])/1024:.1f} KiB, copied by the AI space)'); print(f'  Boot stack  : 0x{d[\"stack_bottom\"]:08X} - 0x{d[\"stack_top\"]:08X}'); print('  Page info   : 1 byte per page, right after the boot stack')"
	@echo ""
	@echo "================================"
