CC = riscv64-unknown-elf-gcc
AS = riscv64-unknown-elf-gcc
LD = riscv64-unknown-elf-ld

CFLAGS = -march=rv64g -mabi=lp64d -mcmodel=medany \
         -ffreestanding -fno-pie -fno-pic \
         -nostdlib -nostartfiles -nodefaultlibs

all: knocos.elf

knocos.elf: boot/boot.o kernel/main.o kernel/memory.o
	$(LD) -T boot/linker.ld -o knocos.elf \
	boot/boot.o kernel/main.o kernel/memory.o

boot/boot.o: boot/boot.S
	$(AS) $(CFLAGS) -c -o boot/boot.o boot/boot.S

kernel/main.o: kernel/main.c
	$(CC) $(CFLAGS) -c -o kernel/main.o kernel/main.c

kernel/memory.o: kernel/memory.c
	$(CC) $(CFLAGS) -c -o kernel/memory.o kernel/memory.c

run: knocos.elf
	env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin qemu-system-riscv64 \
	-machine virt \
	-bios none \
	-kernel knocos.elf \
	-nographic