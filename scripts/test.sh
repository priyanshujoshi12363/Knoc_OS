#!/usr/bin/env bash
set -u

KERNEL=${KERNEL:-knocos.elf}
TIMEOUT=${TIMEOUT:-15}
LOG=$(mktemp)
DISK=$(mktemp)
trap 'rm -f "$LOG" "$DISK"' EXIT

FAILED=0

dd if=/dev/zero of="$DISK" bs=512 count=2048 status=none
printf 'Hello from the host!' | dd of="$DISK" conv=notrunc status=none

boot() {
    (sleep 2; printf 'knocos-echo-test\r'; sleep 1; printf '\004'; sleep 2) |
        timeout "$TIMEOUT" env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        qemu-system-riscv64 -machine virt -bios none -nographic \
        -global virtio-mmio.force-legacy=false \
        -drive file="$DISK",if=none,format=raw,id=disk0 \
        -device virtio-blk-device,drive=disk0,bus=virtio-mmio-bus.0 \
        -kernel "$KERNEL" > "$LOG" 2>&1
    STATUS=$?

    if [ "$STATUS" -eq 124 ]; then
        echo "  FAIL QEMU did not power off within ${TIMEOUT}s"
        FAILED=1
    elif [ "$STATUS" -ne 0 ]; then
        echo "  FAIL QEMU exited with status $STATUS"
        FAILED=1
    fi

    if grep -q "\[PANIC\]" "$LOG"; then
        echo "  FAIL kernel panic"
        FAILED=1
    fi
}

check() {
    for line in "$@"; do
        if grep -qF "$line" "$LOG"; then
            echo "  ok   $line"
        else
            echo "  MISS $line"
            FAILED=1
        fi
    done
}

show_log_on_failure() {
    if [ "$FAILED" -ne 0 ]; then
        echo
        echo "----- QEMU output -----"
        cat "$LOG"
        echo "RESULT: FAIL"
        exit 1
    fi
}

echo "Boot 1: self-tests"
boot
check \
    "Supervisor interrupts enabled" \
    "Page memory initialized" \
    "Sv39 enabled" \
    "Kernel heap 4.0 stress test passed" \
    "Supervisor timer interrupts verified" \
    "Supervisor trap handler verified" \
    "Device ready: uart0 (IRQ 10)" \
    "Device ready: power0" \
    "Device ready: disk0 (IRQ 1)" \
    "Device table verified" \
    "Disk write/read test passed" \
    "Disk says: Hello from the host!" \
    "Disk boot count: 1" \
    "Scheduler started" \
    "AI-aware scheduling verified" \
    "Preemption verified" \
    "Interactive response verified" \
    "All self-tests passed" \
    "knocos-echo-test" \
    "Powering off"
show_log_on_failure

echo "Boot 2: disk data survives a reboot"
boot
check \
    "Disk boot count: 2" \
    "Powering off"
show_log_on_failure

echo "RESULT: PASS"
