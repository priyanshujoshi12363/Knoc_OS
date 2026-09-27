#!/usr/bin/env bash
set -eu

ROOT=build/linux/root
CACHE=build/linux/debs
PACKAGES=(
    "20260920T023840Z pool/main/g/glibc libc6_2.43-6_riscv64.deb f9f28a43d9828036be801e0c39f1ae1dbd07f0485829ccccc7ffe3f61fbf7621"
    "20260921T145302Z pool/main/g/gcc-16 libgcc-s1_16.2.0-3_riscv64.deb 6e04c1f473568302111dcdb2764cb5589d7da6edade5ed21117d7ffb2f89241f"
    "20260921T145302Z pool/main/g/gcc-16 libstdc++6_16.2.0-3_riscv64.deb 84af96e4f6dce8532547067983389904bf2e7fc5820345910136402ed5da22ce"
    "20260704T082931Z pool/main/n/ncurses libtinfo6_6.6+20260608-2_riscv64.deb e671ff7f52c19bd498ee3838a015cbd2504e247727463513378e61dbe7d27b52"
    "20260214T023651Z pool/main/r/readline libreadline8t64_8.3-4_riscv64.deb 4fd6f770cc9b364a2d226aa2dc9e7f49c77b03e22f6776215f01c58079757cbd"
    "20260831T145707Z pool/main/l/lua5.4 liblua5.4-0_5.4.9-1_riscv64.deb 34ea025bfdc9f39f6da3f8862c922b47353d4cae299877e82197c4d1e873939c"
    "20260831T145707Z pool/main/l/lua5.4 lua5.4_5.4.9-1_riscv64.deb 3d7951d396528e5f916f73df3b1602131796828afa854ecced0aa5847da52ac0"
    "20260817T142505Z pool/main/b/busybox busybox_1.38.0-3+b1_riscv64.deb 3296b6826e0bb48f4a230f322a24ea91810c630235cc462229b31d8ebafcde39"
    "20260905T082352Z pool/main/b/bash bash_5.3-4_riscv64.deb fa8d3bfb0d4ef2974eaf8f2fca6eff084023e18b834a60c35b87d05aa4fcfdd7"
)

if [ -f "$ROOT/.complete" ]; then
    exit 0
fi

mkdir -p "$CACHE"
rm -rf "$ROOT"
mkdir -p "$ROOT"

for entry in "${PACKAGES[@]}"; do
    read -r stamp pool name sha <<< "$entry"
    file="$CACHE/$name"
    quoted=${name//+/%2B}

    if [ ! -f "$file" ] || ! echo "$sha  $file" | sha256sum -c --status; then
        curl -fsSL "https://deb.debian.org/debian/$pool/$quoted" -o "$file" ||
            curl -fsSL "https://snapshot.debian.org/archive/debian/$stamp/$pool/$quoted" -o "$file"
    fi

    if ! echo "$sha  $file" | sha256sum -c --status; then
        echo "get-linux-base: $name failed its checksum" >&2
        exit 1
    fi

    python3 - "$file" "$ROOT" <<'PY'
import io, os, sys, tarfile

data = open(sys.argv[1], "rb").read()
root = sys.argv[2]
position = 8
while position < len(data):
    name = data[position:position + 16].decode().strip().rstrip("/")
    size = int(data[position + 48:position + 58].decode().strip())
    body = data[position + 60:position + 60 + size]
    position += 60 + size + (size & 1)
    if not name.startswith("data.tar"):
        continue
    with tarfile.open(fileobj=io.BytesIO(body)) as archive:
        for member in archive.getmembers():
            path = os.path.normpath(member.name)
            if path.startswith("usr/share") or path == "." or path.startswith(".."):
                continue
            target = os.path.join(root, path)
            if member.isdir():
                os.makedirs(target, exist_ok=True)
            elif member.issym():
                os.makedirs(os.path.dirname(target), exist_ok=True)
                if os.path.lexists(target):
                    os.remove(target)
                os.symlink(member.linkname, target)
            elif member.isfile():
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as out:
                    out.write(archive.extractfile(member).read())
                os.utime(target, (member.mtime, member.mtime))
PY
done

mkdir -p "$ROOT/lib"
ln -sf /usr/lib/riscv64-linux-gnu/ld-linux-riscv64-lp64d.so.1 "$ROOT/lib/ld-linux-riscv64-lp64d.so.1"
touch "$ROOT/.complete"
echo "Debian RISC-V base (glibc 2.43, Lua 5.4, BusyBox 1.38, bash 5.3) saved to $ROOT"
