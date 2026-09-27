#!/usr/bin/env bash
set -eu

OUT=${1:-docs/screenshots}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

./scripts/mkdisk.sh "$WORK/disk.img" > /dev/null
python3 tools/knocfs.py put-text "$WORK/disk.img" /home/notes.txt "Groceries: milk, eggs, bread. Call the bank about the invoice." > /dev/null
python3 tools/knocfs.py mkdir "$WORK/disk.img" /home/Documents > /dev/null
printf 'theme=graphite\naccent=ember\nwallpaper=ridge\nscale=100\n' > "$WORK/desktop.conf"
python3 tools/knocfs.py put "$WORK/disk.img" "$WORK/desktop.conf" /etc/desktop.conf > /dev/null

shoot() {
    rm -f "$WORK/qmp.sock"
    QMP_SOCKET="$WORK/qmp.sock" python3 scripts/drive.py 400 "$WORK/log.txt" "$WORK/disk.img" knocos.elf "$@" > /dev/null
}

EARLY_SHOT="4.5:$WORK/boot.ppm" shoot "@wait 8" \
    "@keys {meta_l+e}" "@wait 4" "@keys {meta_l+t}" "@wait 5" "@drag 250 80 610 290" "@wait 4" \
    "@keys cpus{ret}" "@wait 4" "@keys lua5.4 -e 'print(2^10)'{ret}" "@wait 5" \
    "@keys bash -c 'for i in 1 2 3; do echo n\$i; done'{ret}" "@wait 8" "@move 1150 150" "@wait 2" "@shot $WORK/desktop.ppm" \
    "@keys {meta_l}" "@wait 2" "@keys groceries" "@wait 15" "@shot $WORK/knocbar.ppm" "@keys {esc}" "@wait 2" \
    "@keys {meta_l+a}" "@wait 2" "@keys move /home/notes.txt to /home/Documents{ret}" "@wait 8" "@shot $WORK/assist.ppm" \
    "@click 942 326" "@wait 4"

./scripts/mkdisk.sh "$WORK/disk.img" > /dev/null
python3 tools/knocfs.py mkdir "$WORK/disk.img" /home/Documents > /dev/null
python3 tools/knocfs.py put "$WORK/disk.img" "$WORK/desktop.conf" /etc/desktop.conf > /dev/null

shoot "@wait 8" "@keys {meta_l}" "@wait 1" "@keys settings{ret}" "@wait 6" "@move 1150 150" "@wait 2" "@shot $WORK/settings.ppm" \
    "@click 505 164" "@wait 8" "@click 398 251" "@wait 8" "@click 529 350" "@wait 10" "@keys {meta_l+q}" "@wait 3" \
    "@keys {meta_l+e}" "@wait 4" "@keys {meta_l+t}" "@wait 5" "@drag 250 80 610 290" "@wait 4" \
    "@keys ls /home/Downloads{ret}" "@wait 6" "@move 1150 150" "@wait 2" "@shot $WORK/paper.ppm"

for name in boot desktop knocbar assist settings paper; do
    if [ -f "$WORK/$name.ppm" ]; then
        python3 -c "from PIL import Image; Image.open('$WORK/$name.ppm').save('$OUT/$name.png', optimize=True)" 2>/dev/null ||
            cp "$WORK/$name.ppm" "$OUT/$name.ppm"
        echo "saved $OUT/$name.png"
    fi
done
