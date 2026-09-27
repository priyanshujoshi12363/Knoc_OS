#!/usr/bin/env bash
set -eu

OUT=build/linux/busybox
DEB=busybox-static_1.37.0-6+b9_riscv64.deb
SHA256=4add476d2b185c5b487c285b38d790f0881a7e4a8e2ddde835f917e770eb8633
URLS=(
    "https://deb.debian.org/debian/pool/main/b/busybox/busybox-static_1.37.0-6%2Bb9_riscv64.deb"
    "https://snapshot.debian.org/archive/debian/20260712T202631Z/pool/main/b/busybox/busybox-static_1.37.0-6%2Bb9_riscv64.deb"
)

if [ -f "$OUT" ]; then
    exit 0
fi

mkdir -p build/linux
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT

for url in "${URLS[@]}"; do
    if curl -fsSL "$url" -o "$TMP" && echo "$SHA256  $TMP" | sha256sum -c --status; then
        python3 - "$TMP" "$OUT" <<'PY'
import io, sys, tarfile

data = open(sys.argv[1], "rb").read()
assert data[:8] == b"!<arch>\n"
position = 8
while position < len(data):
    name = data[position:position + 16].decode().strip().rstrip("/")
    size = int(data[position + 48:position + 58].decode().strip())
    body = data[position + 60:position + 60 + size]
    position += 60 + size + (size & 1)
    if name.startswith("data.tar"):
        with tarfile.open(fileobj=io.BytesIO(body)) as archive:
            member = archive.getmember("./usr/bin/busybox")
            open(sys.argv[2], "wb").write(archive.extractfile(member).read())
        break
PY
        chmod +x "$OUT"
        echo "BusyBox 1.37.0 (Debian $DEB) saved to $OUT"
        exit 0
    fi
done

echo "get-busybox: could not download $DEB" >&2
exit 1
