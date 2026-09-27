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

LIBGCC := $(shell $(CC) -march=rv64g -mabi=lp64d -print-libgcc-file-name)

QEMU = env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin qemu-system-riscv64
RAM ?= 2G
QEMU_FLAGS = -machine virt -smp 8 -m $(RAM) -bios none -nographic

DISK = disk.img
DISK_MB ?= 64
KNOCFS = python3 tools/knocfs.py
QEMU_DISK_FLAGS = -global virtio-mmio.force-legacy=false \
                  -drive file=$(DISK),if=none,format=raw,id=disk0 \
                  -device virtio-blk-device,drive=disk0,bus=virtio-mmio-bus.0 \
                  -netdev user,id=net0 -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.1 \
                  -device virtio-rng-device,bus=virtio-mmio-bus.2

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
              kernel/knocfs.o \
              kernel/tty.o \
              kernel/memgraph.o \
              kernel/telemetry.o \
              kernel/virtio_net.o \
              kernel/net.o \
              kernel/virtio_rng.o \
              kernel/rtc.o \
              kernel/cpu.o \
              kernel/linux.o \
              kernel/aispace.o \
              kernel/virtio_gpu.o \
              kernel/fbcon.o \
              kernel/screen.o \
              kernel/input.o \
              kernel/virtio_input.o \
              kernel/pty.o

TIMER_OBJS = timer/timer.o

USER_PROGRAMS = hello badcall noperm hog bigmem crash spy files modelcheck knocsh counter organize leak spin diskload quiet spawner filler recorder healthd ask agent chat organized session
USER_LIB_OBJS = user/crt0.o user/ulib.o user/nn.o
LIBC_OBJS = user/libc/stdio.o user/libc/stdlib.o user/libc/string.o user/libc/ctype.o user/libc/math.o user/libc/misc.o user/libc/posix.o user/libc/pthread.o user/libc/setjmp.o
LIBC_CRT = user/libc/crt1.o
LIBC_PROGRAMS = libctest calc net ping fetch web date threadtest knocnet knocnetd find index indexd gfx
SEARCH_PROGRAMS = find index indexd
GUI_PROGRAMS = gfx
GUI_OBJS = user/draw.o user/wallpaper.o user/stb.o
DESKTOP_OBJS = user/desktop/main.o user/desktop/ui.o user/desktop/term.o user/desktop/apps.o user/desktop/knoc.o
HTTP_PROGRAMS = fetch web
KNOCNET_PROGRAMS = knocnet knocnetd
USER_ELFS = $(USER_PROGRAMS:%=user/%.elf) $(LIBC_PROGRAMS:%=user/%.elf) user/linuxtest.elf user/desktop.elf
USER_OBJS = $(USER_LIB_OBJS) $(GUI_OBJS) $(DESKTOP_OBJS) user/http.o user/knocnet_proto.o user/search.o user/embed.o user/rag.o user/llm.o user/assist.o user/learn.o $(LIBC_OBJS) $(LIBC_CRT) $(USER_PROGRAMS:%=user/%.o) $(LIBC_PROGRAMS:%=user/%.o)

DEPS = $(KERNEL_OBJS:.o=.d) $(TIMER_OBJS:.o=.d) $(USER_OBJS:.o=.d) $(DESKTOP_AI_OBJS:.o=.d)

.PHONY: all clean run run-gui test timer-test size pages reset-disk sync-programs put ls tcc-sdk linux-apps


all: knocos.elf tcc-sdk

linux-apps:
	./scripts/get-busybox.sh
	./scripts/get-linux-base.sh

TCC_DIR = third_party/tinycc
TCC_BUILD = build/tcc
TCC_SDK = $(TCC_BUILD)/sdk
TCC_DEFS = -DONE_SOURCE=1 -DTCC_TARGET_RISCV64 -DTCC_KNOCOS -DCONFIG_TCC_STATIC -DCONFIG_TCC_SEMLOCK=0 \
           -DCONFIG_TCC_BACKTRACE=0 -DCONFIG_TCC_BCHECK=0 '-DCONFIG_TCCDIR="/lib/tcc"' \
           '-DCONFIG_TCC_SYSINCLUDEPATHS="{B}/include:/include"' '-DCONFIG_TCC_LIBPATHS="/lib"' \
           '-DCONFIG_TCC_CRTPREFIX="/lib"' '-DCONFIG_TCC_ELFINTERP="-"'
TCC_SOURCES = $(wildcard $(TCC_DIR)/*.c $(TCC_DIR)/*.h)
KNOC_TCC = $(TCC_BUILD)/knoc-tcc
KNOC_TCC_FLAGS = -nostdinc -I $(TCC_DIR)/include -I user/libc/include
LIBC_HEADERS = $(wildcard user/libc/include/*.h user/libc/include/sys/*.h)
SDK_LIBC_OBJS = $(patsubst %,$(TCC_SDK)/%.o,stdio stdlib string ctype math misc posix pthread setjmp ulib)
SDK_TCC1_OBJS = $(patsubst %,$(TCC_SDK)/tcc1-%.o,lib-arm64 builtin stdatomic alloca atomic)
SDK_FILES = $(TCC_SDK)/crt1.o $(TCC_SDK)/crti.o $(TCC_SDK)/crtn.o $(TCC_SDK)/libc.a $(TCC_SDK)/libtcc1.a

$(TCC_BUILD)/tccdefs_.h: $(TCC_DIR)/include/tccdefs.h $(TCC_DIR)/conftest.c
	@mkdir -p $(TCC_BUILD)
	gcc -DC2STR -o $(TCC_BUILD)/c2str $(TCC_DIR)/conftest.c
	$(TCC_BUILD)/c2str $< $@

$(KNOC_TCC): $(TCC_SOURCES) $(TCC_BUILD)/tccdefs_.h
	gcc -O2 -w -I$(TCC_BUILD) $(TCC_DEFS) -o $@ $(TCC_DIR)/tcc.c -lm

$(TCC_BUILD)/tcc.o: $(TCC_SOURCES) $(TCC_BUILD)/tccdefs_.h $(LIBC_HEADERS)
	$(CC) -march=rv64g -mabi=lp64d -mcmodel=medany -ffreestanding -fno-pie -fno-pic -nostdlib -O2 -w \
		-isystem user/libc/include -I$(TCC_BUILD) $(TCC_DEFS) -c -o $@ $(TCC_DIR)/tcc.c

user/tcc.elf: $(TCC_BUILD)/tcc.o $(LIBC_CRT) $(LIBC_OBJS) user/ulib.o user/linker.ld
	$(LD) -T user/linker.ld -s -o $@ $(LIBC_CRT) $(TCC_BUILD)/tcc.o $(LIBC_OBJS) user/ulib.o $(LIBGCC)

$(TCC_SDK)/%.o: user/libc/%.c $(KNOC_TCC) $(LIBC_HEADERS)
	@mkdir -p $(TCC_SDK)
	$(KNOC_TCC) $(KNOC_TCC_FLAGS) -c $< -o $@

$(TCC_SDK)/setjmp.o: user/libc/setjmp.S $(KNOC_TCC)
	@mkdir -p $(TCC_SDK)
	$(KNOC_TCC) $(KNOC_TCC_FLAGS) -c $< -o $@

$(TCC_SDK)/ulib.o: user/ulib.c user/ulib.h kernel/syscall_abi.h $(KNOC_TCC)
	@mkdir -p $(TCC_SDK)
	$(KNOC_TCC) $(KNOC_TCC_FLAGS) -c $< -o $@

$(TCC_SDK)/tcc1-%.o: $(TCC_DIR)/lib/%.c $(KNOC_TCC)
	@mkdir -p $(TCC_SDK)
	$(KNOC_TCC) $(KNOC_TCC_FLAGS) -c $< -o $@

$(TCC_SDK)/tcc1-%.o: $(TCC_DIR)/lib/%.S $(KNOC_TCC)
	@mkdir -p $(TCC_SDK)
	$(KNOC_TCC) $(KNOC_TCC_FLAGS) -c $< -o $@

$(TCC_SDK)/crti.o $(TCC_SDK)/crtn.o: $(KNOC_TCC)
	@mkdir -p $(TCC_SDK)
	printf '\n' > $(TCC_BUILD)/empty.c
	$(KNOC_TCC) -c $(TCC_BUILD)/empty.c -o $@

$(TCC_SDK)/libc.a: $(SDK_LIBC_OBJS)
	rm -f $@
	$(KNOC_TCC) -ar rcs $@ $^

$(TCC_SDK)/libtcc1.a: $(SDK_TCC1_OBJS)
	rm -f $@
	$(KNOC_TCC) -ar rcs $@ $^

tcc-sdk: user/tcc.elf $(SDK_FILES)

knocos.elf: $(KERNEL_OBJS) boot/linker.ld
	$(LD) -T boot/linker.ld -o knocos.elf $(KERNEL_OBJS)

user/%.elf: user/%.o $(USER_LIB_OBJS) user/linker.ld
	$(LD) -T user/linker.ld -s -o $@ $(USER_LIB_OBJS) $<

user/ask.elf: user/ask.o user/llm.o user/rag.o user/embed.o $(USER_LIB_OBJS) user/linker.ld
	$(LD) -T user/linker.ld -s -o $@ $(USER_LIB_OBJS) user/ask.o user/llm.o user/rag.o user/embed.o

user/agent.elf: user/agent.o user/assist.o user/llm.o user/rag.o user/embed.o $(USER_LIB_OBJS) user/linker.ld
	$(LD) -T user/linker.ld -s -o $@ $(USER_LIB_OBJS) user/agent.o user/assist.o user/llm.o user/rag.o user/embed.o

$(LIBC_OBJS) $(LIBC_CRT) $(LIBC_PROGRAMS:%=user/%.o) $(GUI_OBJS) user/http.o user/knocnet_proto.o user/search.o: CFLAGS += -isystem user/libc/include

user/draw.o user/wallpaper.o: CFLAGS += -O2

KNOC_NAMES = -Dopen=knoc_open -Dstat=knoc_stat
DESKTOP_AI_OBJS = build/desktop/assist.o build/desktop/llm.o build/desktop/rag.o build/desktop/embed.o build/desktop/nn.o

$(DESKTOP_OBJS): CFLAGS += -isystem user/libc/include -O2 $(KNOC_NAMES)

build/desktop/%.o: user/%.c
	@mkdir -p build/desktop
	$(CC) $(CFLAGS) $(KNOC_NAMES) -O3 -funroll-loops -c -o $@ $<

user/desktop.elf: $(DESKTOP_OBJS) $(GUI_OBJS) $(DESKTOP_AI_OBJS) $(LIBC_CRT) $(LIBC_OBJS) user/ulib.o user/linker.ld
	$(LD) -T user/linker.ld -s -o $@ $(LIBC_CRT) $(DESKTOP_OBJS) $(GUI_OBJS) $(DESKTOP_AI_OBJS) $(LIBC_OBJS) user/ulib.o $(LIBGCC)

user/stb.o: user/stb.c third_party/stb/stb_truetype.h third_party/stb/stb_image.h
	$(CC) -march=rv64g -mabi=lp64d -mcmodel=medany -ffreestanding -fno-pie -fno-pic -nostdlib -O2 -w \
		-isystem user/libc/include -c -o $@ $<

$(GUI_PROGRAMS:%=user/%.elf): user/%.elf: user/%.o $(GUI_OBJS) $(LIBC_CRT) $(LIBC_OBJS) user/ulib.o user/linker.ld
	$(LD) -T user/linker.ld -s -o $@ $(LIBC_CRT) $< $(GUI_OBJS) $(LIBC_OBJS) user/ulib.o $(LIBGCC)

$(filter-out $(HTTP_PROGRAMS:%=user/%.elf) $(KNOCNET_PROGRAMS:%=user/%.elf) $(SEARCH_PROGRAMS:%=user/%.elf) $(GUI_PROGRAMS:%=user/%.elf),$(LIBC_PROGRAMS:%=user/%.elf)): user/%.elf: user/%.o $(LIBC_CRT) $(LIBC_OBJS) user/ulib.o user/linker.ld
	$(LD) -T user/linker.ld -s -o $@ $(LIBC_CRT) $< $(LIBC_OBJS) user/ulib.o $(LIBGCC)

BEARSSL_DIR = third_party/bearssl
BEARSSL_SOURCES = $(wildcard $(BEARSSL_DIR)/src/*.c $(BEARSSL_DIR)/src/*/*.c)
BEARSSL_OBJS = $(BEARSSL_SOURCES:$(BEARSSL_DIR)/src/%.c=build/bearssl/%.o)
BEARSSL_LIB = build/bearssl/libbearssl.a
BEARSSL_DEFS = -DBR_USE_UNIX_TIME=0 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_RDRAND=0 -DBR_64=1

build/bearssl/%.o: $(BEARSSL_DIR)/src/%.c
	@mkdir -p $(dir $@)
	@$(CC) -march=rv64g -mabi=lp64d -mcmodel=medany -ffreestanding -fno-pie -fno-pic -nostdlib -O2 -w \
		-isystem user/libc/include -I$(BEARSSL_DIR)/inc -I$(BEARSSL_DIR)/src $(BEARSSL_DEFS) -c -o $@ $<

$(BEARSSL_LIB): $(BEARSSL_OBJS)
	rm -f $@
	riscv64-unknown-elf-ar rcs $@ $^

user/http.o user/knocnet_proto.o: CFLAGS += -O2 -I$(BEARSSL_DIR)/inc

user/embed.o user/search.o: CFLAGS += -O2

$(SEARCH_PROGRAMS:%=user/%.elf): user/%.elf: user/%.o user/search.o user/embed.o $(LIBC_CRT) $(LIBC_OBJS) user/ulib.o user/linker.ld
	$(LD) -T user/linker.ld -s -o $@ $(LIBC_CRT) $< user/search.o user/embed.o $(LIBC_OBJS) user/ulib.o $(LIBGCC)

$(KNOCNET_PROGRAMS:%=user/%.elf): user/%.elf: user/%.o user/knocnet_proto.o $(BEARSSL_LIB) $(LIBC_CRT) $(LIBC_OBJS) user/ulib.o user/linker.ld
	$(LD) -T user/linker.ld -s -o $@ $(LIBC_CRT) $< user/knocnet_proto.o $(BEARSSL_LIB) $(LIBC_OBJS) user/ulib.o $(LIBGCC)

$(HTTP_PROGRAMS:%=user/%.elf): user/%.elf: user/%.o user/http.o $(BEARSSL_LIB) $(LIBC_CRT) $(LIBC_OBJS) user/ulib.o user/linker.ld
	$(LD) -T user/linker.ld -s -o $@ $(LIBC_CRT) $< user/http.o $(BEARSSL_LIB) $(LIBC_OBJS) user/ulib.o $(LIBGCC)

user/organize.elf: user/organize.o user/learn.o $(USER_LIB_OBJS) user/linker.ld
	$(LD) -T user/linker.ld -s -o $@ $(USER_LIB_OBJS) user/organize.o user/learn.o

user/chat.elf: user/chat.o user/assist.o user/llm.o user/rag.o user/embed.o $(USER_LIB_OBJS) user/linker.ld
	$(LD) -T user/linker.ld -s -o $@ $(USER_LIB_OBJS) user/chat.o user/assist.o user/llm.o user/rag.o user/embed.o

user/linuxtest.elf: user/linux/linuxtest.c user/linux/linux.ld
	$(CC) -march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding -fno-pie -fno-pic -nostdlib -static -O2 \
		-Wall -Wextra -Werror -fno-builtin -T user/linux/linux.ld -s -o $@ $<

kernel/programs.o: $(USER_ELFS)

timer.elf: $(TIMER_OBJS) timer/linker.ld
	$(LD) -T timer/linker.ld -o timer.elf $(TIMER_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(AS) $(CFLAGS) -c -o $@ $<

kernel/main.o: VERSION

build/mkfont: tools/mkfont.c third_party/stb/stb_truetype.h
	@mkdir -p build
	gcc -O2 -w -o $@ tools/mkfont.c -lm

build/console_font.h: build/mkfont third_party/fonts/JetBrainsMono-Regular.ttf
	build/mkfont third_party/fonts/JetBrainsMono-Regular.ttf 16 $@

build/splash_font.h: build/mkfont third_party/fonts/HankenGrotesk-SemiBold.ttf
	build/mkfont third_party/fonts/HankenGrotesk-SemiBold.ttf 46 $@ splash

kernel/fbcon.o: build/console_font.h build/splash_font.h
kernel/fbcon.o: CFLAGS += -Ibuild

user/ask.o user/llm.o: CFLAGS += -O3 -funroll-loops

-include $(DEPS)

clean:
	rm -f knocos.elf timer.elf $(KERNEL_OBJS) $(TIMER_OBJS) $(USER_OBJS) $(USER_ELFS) $(DEPS) user/tcc.elf
	rm -rf build

$(DISK): | $(USER_ELFS) tcc-sdk
	./scripts/mkdisk.sh $(DISK) $(DISK_MB)

reset-disk: $(USER_ELFS) tcc-sdk
	./scripts/mkdisk.sh $(DISK) $(DISK_MB)

# Copy freshly built programs into /bin, keeping every other file on the disk
sync-programs: $(USER_ELFS) tcc-sdk $(DISK)
	@for program in $(USER_PROGRAMS) $(LIBC_PROGRAMS) linuxtest desktop; do \
		$(KNOCFS) put $(DISK) user/$$program.elf /bin/$$program 2>/dev/null || \
			{ echo "$(DISK) has no KnocFS: run make reset-disk"; break; }; \
	done
	@$(KNOCFS) mkdir $(DISK) /etc /etc/apps 2>/dev/null || true
	@./scripts/sdk.sh $(DISK)
	@./scripts/etc.sh $(DISK)
	@[ ! -f models/embed/knocembed.knm ] || $(KNOCFS) put $(DISK) models/embed/knocembed.knm /models/knocembed.knm
	@$(KNOCFS) mkdir $(DISK) /fonts 2>/dev/null || true
	@for font in third_party/fonts/*.ttf; do $(KNOCFS) put $(DISK) $$font /fonts/$$(basename $$font); done
	@[ ! -f build/linux/busybox ] || $(KNOCFS) put $(DISK) build/linux/busybox /bin/busybox
	@[ ! -f build/linux/root/.complete ] || $(KNOCFS) put-tree $(DISK) build/linux/root /
	@for manifest in apps/*.app; do \
		$(KNOCFS) put $(DISK) $$manifest /etc/apps/$$(basename $$manifest) 2>/dev/null || break; \
	done

# make put FILE=model.gguf DEST=/models/model.gguf
put: $(DISK)
	$(KNOCFS) put $(DISK) $(FILE) $(DEST)

# make ls DIR=/models
ls: $(DISK)
	$(KNOCFS) ls $(DISK) $(or $(DIR),/)

run: knocos.elf sync-programs
	$(QEMU) $(QEMU_FLAGS) $(QEMU_DISK_FLAGS) -kernel knocos.elf

GUI_DISPLAY ?= gtk,zoom-to-fit=off
GUI_ENV = env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin HOME="$$HOME" DISPLAY="$$DISPLAY" \
          WAYLAND_DISPLAY="$$WAYLAND_DISPLAY" XAUTHORITY="$$XAUTHORITY" XDG_RUNTIME_DIR="$$XDG_RUNTIME_DIR" \
          XDG_SESSION_TYPE="$$XDG_SESSION_TYPE" DBUS_SESSION_BUS_ADDRESS="$$DBUS_SESSION_BUS_ADDRESS" qemu-system-riscv64
run-gui: knocos.elf sync-programs
	$(GUI_ENV) -machine virt -smp 8 -m $(RAM) -bios none -serial mon:stdio -display $(GUI_DISPLAY) \
		$(QEMU_DISK_FLAGS) -device virtio-gpu-device,xres=1280,yres=800,bus=virtio-mmio-bus.3 \
		-device virtio-keyboard-device,bus=virtio-mmio-bus.4 -device virtio-tablet-device,bus=virtio-mmio-bus.5 -kernel knocos.elf

test: knocos.elf tcc-sdk
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
