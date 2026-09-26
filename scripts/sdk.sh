#!/usr/bin/env bash
set -eu

DISK=$1
KNOCFS="python3 tools/knocfs.py"
SDK=build/tcc/sdk

if [ ! -f "$SDK/libc.a" ] || [ ! -f user/tcc.elf ]; then
    exit 0
fi

$KNOCFS mkdir "$DISK" /lib /lib/tcc /include
$KNOCFS put "$DISK" user/tcc.elf /bin/tcc
$KNOCFS put-many "$DISK" /lib "$SDK/crt1.o" "$SDK/crti.o" "$SDK/crtn.o" "$SDK/libc.a" "$SDK/libtcc1.a"
$KNOCFS put-many "$DISK" /lib/tcc/include third_party/tinycc/include/*.h
$KNOCFS put-many "$DISK" /include user/libc/include/*.h
$KNOCFS put-many "$DISK" /include/sys user/libc/include/sys/*.h
