CC = riscv64-unknown-elf-gcc
AS = riscv64-unknown-elf-gcc
LD = riscv64-unknown-elf-ld

CFLAGS = -march=rv64g -mabi=lp64d -mcmodel=medany \
         -ffreestanding -fno-pie -fno-pic \
         -nostdlib -nostartfiles -nodefaultlibs

all: knocos.elf

knocos.elf: boot/boot.o kernel/main.o kernel/memory.o kernel/logging.o kernel/page.o
	$(LD) -T boot/linker.ld -o knocos.elf \
	boot/boot.o kernel/main.o kernel/memory.o kernel/logging.o kernel/page.o

boot/boot.o: boot/boot.S
	$(AS) $(CFLAGS) -c -o boot/boot.o boot/boot.S

kernel/main.o: kernel/main.c
	$(CC) $(CFLAGS) -c -o kernel/main.o kernel/main.c

kernel/memory.o: kernel/memory.c
	$(CC) $(CFLAGS) -c -o kernel/memory.o kernel/memory.c

kernel/logging.o: kernel/logging.c kernel/logging.h
	$(CC) $(CFLAGS) -c -o kernel/logging.o kernel/logging.c

kernel/page.o: kernel/page.c kernel/page.h
	$(CC) $(CFLAGS) -c -o kernel/page.o kernel/page.c

clean:
	rm -f knocos.elf \
	      boot/boot.o \
	      kernel/main.o \
	      kernel/memory.o \
	      kernel/logging.o \
	      kernel/page.o

run: knocos.elf
	env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin qemu-system-riscv64 \
	-machine virt \
	-bios none \
	-kernel knocos.elf \
	-nographic

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