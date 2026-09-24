#!/usr/bin/env bash
# Creates a KnocOS disk image: the host test text in sector 0, a KnocFS
# filesystem with the user programs in /bin and a test model in /models.
set -eu

DISK=$1
SIZE_MB=${2:-64}
KNOCFS="python3 tools/knocfs.py"

case "$SIZE_MB" in
    ''|*[!0-9]*)
        echo "mkdisk: size must be a whole number of MiB, like 64 or 4096 (got '$SIZE_MB')" >&2
        exit 1
        ;;
esac

if [ "$SIZE_MB" -lt 4 ]; then
    echo "mkdisk: the disk must be at least 4 MiB" >&2
    exit 1
fi

rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=0 seek="$SIZE_MB" status=none
printf 'Hello from the host!' | dd of="$DISK" conv=notrunc status=none

$KNOCFS format "$DISK" > /dev/null
$KNOCFS mkdir "$DISK" /bin /models /home /tmp

for program in user/*.elf; do
    $KNOCFS put "$DISK" "$program" "/bin/$(basename "$program" .elf)"
done

$KNOCFS put-text "$DISK" /hello.txt "Hello from a file on KnocFS!"
$KNOCFS make-test-model "$DISK" /models/test-model.bin 8

if [ -f models/filenet/filenet.knm ]; then
    $KNOCFS put "$DISK" models/filenet/filenet.knm /models/filenet.knm
fi

if [ -f models/health/health.knm ]; then
    $KNOCFS put "$DISK" models/health/health.knm /models/health.knm
fi

if [ -f models/llm/qwen.kllm ] && [ "$SIZE_MB" -ge 1024 ]; then
    $KNOCFS put "$DISK" models/llm/qwen.kllm /models/qwen.kllm
fi

SAMPLES=$(mktemp -d)
python3 scripts/sample_downloads.py "$SAMPLES"
$KNOCFS mkdir "$DISK" /home/Downloads
for sample in "$SAMPLES"/*; do
    $KNOCFS put "$DISK" "$sample" "/home/Downloads/$(basename "$sample")"
done
rm -rf "$SAMPLES"
