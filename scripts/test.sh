#!/usr/bin/env bash
set -u

KERNEL=${KERNEL:-knocos.elf}
TIMEOUT=${TIMEOUT:-15}
LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT

EXPECTED=(
    "Supervisor interrupts enabled"
    "Page memory initialized"
    "Sv39 enabled"
    "Kernel heap 4.0 stress test passed"
    "Supervisor timer interrupts verified"
    "Supervisor trap handler verified"
    "UART input interrupts enabled"
    "All self-tests passed"
    "knocos-echo-test"
    "Powering off"
)

(sleep 2; printf 'knocos-echo-test\r'; sleep 1; printf '\004'; sleep 2) |
    timeout "$TIMEOUT" env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin \
    qemu-system-riscv64 -machine virt -bios none -nographic -kernel "$KERNEL" \
    > "$LOG" 2>&1
STATUS=$?

FAILED=0

if [ "$STATUS" -eq 124 ]; then
    echo "FAIL: QEMU did not power off within ${TIMEOUT}s"
    FAILED=1
elif [ "$STATUS" -ne 0 ]; then
    echo "FAIL: QEMU exited with status $STATUS"
    FAILED=1
fi

if grep -q "\[PANIC\]" "$LOG"; then
    echo "FAIL: kernel panic"
    FAILED=1
fi

for line in "${EXPECTED[@]}"; do
    if grep -qF "$line" "$LOG"; then
        echo "  ok   $line"
    else
        echo "  MISS $line"
        FAILED=1
    fi
done

if [ "$FAILED" -ne 0 ]; then
    echo
    echo "----- QEMU output -----"
    cat "$LOG"
    echo "RESULT: FAIL"
    exit 1
fi

echo "RESULT: PASS"
