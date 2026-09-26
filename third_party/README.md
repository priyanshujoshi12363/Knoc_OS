# Third-party code

## TinyCC (`tinycc/`)

- Source: https://repo.or.cz/tinycc.git (mob branch), commit `c982991` (version 0.9.28rc)
- License: GNU LGPL 2.1 (see `tinycc/COPYING`)
- Used for: the C compiler inside KnocOS (`/bin/tcc`) and the host cross compiler that builds its runtime (`build/tcc/knoc-tcc`)

KnocOS changes, all behind `TCC_KNOCOS`:

- `tcc.h`: no `-run` support (KnocOS programs can't make memory executable)
- `riscv64-link.c`: programs start at `0x1000000000`, the start of KnocOS user space
- `libtcc.c`: programs are linked statically by default
- `config.h`: replaced by a minimal KnocOS version (the rest comes from `TCC_DEFS` in the `Makefile`)
