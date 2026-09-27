# Third-party code

## BusyBox (downloaded, not stored here)

- Source: Debian package `busybox-static_1.37.0-6+b9_riscv64.deb` (a static RISC-V Linux build), downloaded by `scripts/get-busybox.sh` from deb.debian.org or snapshot.debian.org and checked with SHA-256
- License: GNU GPL 2 (source: https://sources.debian.org/src/busybox/1:1.37.0-6/)
- Used for: Linux programs on KnocOS: `/bin/busybox` and its tools (`grep`, `sed`, `awk`, `tar`, `vi`, `top`...); KnocOS runs the Debian binary unchanged

## Debian base for dynamic Linux programs (downloaded, not stored here)

- Source: Debian RISC-V packages `libc6` 2.43 (glibc, LGPL 2.1), `libgcc-s1` and `libstdc++6` 16.2 (GPL 3 with the runtime exception), `libtinfo6` 6.6 (ncurses, MIT-style), `libreadline8t64` 8.3 (GPL 3), `lua5.4` and `liblua5.4-0` 5.4.9 (MIT), `busybox` 1.38 (GPL 2), `bash` 5.3 (GPL 3)
- `scripts/get-linux-base.sh` downloads them from deb.debian.org or snapshot.debian.org, checks each with SHA-256 and unpacks them (without `/usr/share`) into `build/linux/root`; `mkdisk` copies that tree onto the disk with its symbolic links
- Source code for every package: https://sources.debian.org/

## BearSSL (`bearssl/`)

- Source: https://www.bearssl.org/git/BearSSL, commit `7bea48e`
- License: MIT (see `bearssl/LICENSE.txt`)
- Used for: HTTPS (TLS 1.0 - 1.2) in `fetch` and `web`, through `user/http.c`
- Only `src/`, `inc/`, the license and the README are kept; no changes to the code. The `Makefile` builds it into `build/bearssl/libbearssl.a` with the system clock and random sources turned off (`BR_USE_UNIX_TIME=0`, `BR_USE_URANDOM=0`, `BR_USE_GETENTROPY=0`): KnocOS passes the time from the `rtc0` clock and random bytes from the `rng0` device itself

## TinyCC (`tinycc/`)

- Source: https://repo.or.cz/tinycc.git (mob branch), commit `c982991` (version 0.9.28rc)
- License: GNU LGPL 2.1 (see `tinycc/COPYING`)
- Used for: the C compiler inside KnocOS (`/bin/tcc`) and the host cross compiler that builds its runtime (`build/tcc/knoc-tcc`)

KnocOS changes, all behind `TCC_KNOCOS`:

- `tcc.h`: no `-run` support (KnocOS programs can't make memory executable)
- `riscv64-link.c`: programs start at `0x1000000000`, the start of KnocOS user space
- `libtcc.c`: programs are linked statically by default
- `config.h`: replaced by a minimal KnocOS version (the rest comes from `TCC_DEFS` in the `Makefile`)

## stb (`stb/`)

- Source: https://github.com/nothings/stb, commit `2c980bb`: `stb_truetype.h` (v1.26) and `stb_image.h` (v2.30), unchanged
- License: public domain or MIT, your choice (at the end of each file)
- Used for: drawing fonts (`stb_truetype`) and reading PNG and JPEG images (`stb_image`) in the graphics library `user/gfx.c`, and at build time by `tools/mkfont.c`, which makes the kernel's console font

## Fonts (`fonts/`)

- JetBrains Mono Regular (https://github.com/JetBrains/JetBrainsMono), Hanken Grotesk (https://github.com/marcologous/hanken-grotesk), Martian Mono (https://github.com/evilmartians/mono)
- License: SIL Open Font License 1.1, no Reserved Font Names (see `fonts/OFL-*.txt`)
- Hanken Grotesk and Martian Mono come as variable fonts from Google Fonts (`ofl/hankengrotesk`, `ofl/martianmono`). They were turned into fixed instances with fontTools `instancer`: Hanken Grotesk Regular (weight 400) and SemiBold (600), Martian Mono Regular (weight 400, width 100)
- Used for: the KnocOS interface (Hanken Grotesk), labels and numbers (Martian Mono), the terminal and the kernel console (JetBrains Mono); they are copied to `/fonts` on the disk
