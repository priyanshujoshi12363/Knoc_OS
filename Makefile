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
QEMU_FLAGS = -machine virt -smp 2 -bios none -nographic

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
              kernel/aispace.o

TIMER_OBJS = timer/timer.o

DEPS = $(KERNEL_OBJS:.o=.d) $(TIMER_OBJS:.o=.d)

.PHONY: all clean run test timer-test size pages reset-disk

all: knocos.elf

knocos.elf: $(KERNEL_OBJS) boot/linker.ld
	$(LD) -T boot/linker.ld -o knocos.elf $(KERNEL_OBJS)

timer.elf: $(TIMER_OBJS) timer/linker.ld
	$(LD) -T timer/linker.ld -o timer.elf $(TIMER_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(AS) $(CFLAGS) -c -o $@ $<

kernel/main.o: VERSION

-include $(DEPS)

clean:
	rm -f knocos.elf timer.elf $(KERNEL_OBJS) $(TIMER_OBJS) $(DEPS)

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
	@echo "  Total: 128 MiB"

pages: knocos.elf
	@echo "================================"
	@echo "        KnocOS Page Info"
	@echo "================================"
	@echo ""
	@echo "Physical Memory:"
	@echo "  RAM Start   : 0x80000000"
	@echo "  RAM End     : 0x88000000"
	@echo "  Total       : 128.00 MiB"
	@echo ""
	@echo "Page Configuration:"
	@echo "  Page Size   : 4096 bytes"
	@echo "  Total Pages : 32768"
	@echo "  Bitmap Size : 4096 bytes"
	@echo ""
	@echo "Page State:"
	@python3 -c "import subprocess; s=subprocess.check_output(['riscv64-unknown-elf-nm','-n','knocos.elf'],text=True); d={line.split()[-1]:int(line.split()[0],16) for line in s.splitlines() if len(line.split())>=3}; first=((d['stack_top']+4095)&~4095-0x80000000)//4096; total=(0x88000000-0x80000000)//4096; used=first; free=total-used; print(f'  Used Pages  : {used}'); print(f'  Free Pages  : {free}'); print(f'  Used Memory : {used*4096/1024:.2f} KiB'); print(f'  Free Memory : {free*4096/1024/1024:.2f} MiB')"
	@echo ""
	@echo "Page Map:"
	@python3 -c "import subprocess; s=subprocess.check_output(['riscv64-unknown-elf-nm','-n','knocos.elf'],text=True); d={line.split()[-1]:int(line.split()[0],16) for line in s.splitlines() if len(line.split())>=3}; first=((d['stack_top']+4095)&~4095-0x80000000)//4096; print(f'  Reserved    : Pages 0 - {first-1}'); print(f'  Available   : Pages {first} - 32767')"
	@echo ""
	@echo "================================"