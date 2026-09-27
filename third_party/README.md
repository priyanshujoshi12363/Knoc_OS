# Third-party code

## BusyBox (downloaded, not stored here)

- Source: Debian package `busybox-static_1.37.0-6+b9_riscv64.deb` (a static RISC-V Linux build), downloaded by `scripts/get-busybox.sh` from deb.debian.org or snapshot.debian.org and checked with SHA-256
- License: GNU GPL 2 (source: https://sources.debian.org/src/busybox/1:1.37.0-6/)
- Used for: Linux programs on KnocOS: `/bin/busybox` and its tools (`grep`, `sed`, `awk`, `tar`, `vi`, `top`...); KnocOS runs the Debian binary unchanged

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
