#!/usr/bin/env bash
set -u

KERNEL=${KERNEL:-knocos.elf}
TIMEOUT=${TIMEOUT:-15}
GUARDIAN_TIMEOUT=${GUARDIAN_TIMEOUT:-60}
HALT_TIMEOUT=${HALT_TIMEOUT:-12}
LOG=$(mktemp)
DISK=$(mktemp)
trap 'rm -f "$LOG" "$DISK"' EXIT

FAILED=0

new_disk() {
    dd if=/dev/zero of="$DISK" bs=512 count=2048 status=none
    printf 'Hello from the host!' | dd of="$DISK" conv=notrunc status=none
}

qemu() {
    timeout "$1" env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        qemu-system-riscv64 -machine virt -smp 2 -m 2G -bios none -nographic \
        -global virtio-mmio.force-legacy=false \
        -drive file="$DISK",if=none,format=raw,id=disk0 \
        -device virtio-blk-device,drive=disk0,bus=virtio-mmio-bus.0 \
        -kernel "$KERNEL" > "$LOG" 2>&1
}

new_disk

boot() {
    (sleep 2; printf 'knocos-echo-test\r'; sleep 1; printf '\004'; sleep 2) | qemu "$TIMEOUT"
    STATUS=$?
    check_status "$TIMEOUT" no-panic
}

check_status() {
    if [ "$1" = "halt-expected" ]; then
        if [ "$STATUS" -ne 124 ]; then
            echo "  FAIL QEMU exited (status $STATUS), but the kernel should stay halted"
            FAILED=1
        fi
    elif [ "$STATUS" -eq 124 ]; then
        echo "  FAIL QEMU did not power off within ${1}s"
        FAILED=1
    elif [ "$STATUS" -ne 0 ]; then
        echo "  FAIL QEMU exited with status $STATUS"
        FAILED=1
    fi

    if [ "$2" = "no-panic" ] && grep -q "\[PANIC\]" "$LOG"; then
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
    "RAM 2048 MiB at 0x0000000080000000, 2 CPUs" \
    "Page memory initialized" \
    "largest block 1024 MiB (buddy allocator)" \
    "Sv39 enabled" \
    "RAM mapped with 2 MiB megapages: 1026" \
    "Spinlock verified" \
    "Buddy allocator verified" \
    "Large memory verified: 1024 MiB block" \
    "Kernel heap 4.0 stress test passed" \
    "Supervisor timer interrupts verified" \
    "Supervisor trap handler verified" \
    "PMP verified: the kernel cannot read the AI space" \
    "AI space online (core 1, 256 MiB protected at 0x0000000090000000)" \
    "Black box: no previous crashes" \
    "Device ready: uart0 (IRQ 10)" \
    "Device ready: power0" \
    "Device ready: disk0 (IRQ 1)" \
    "Device ready: faulty0" \
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

echo "Run 3: fault containment, driver disabling and warm kernel restarts"
new_disk
(
    sleep 4;  printf '\006'
    sleep 2;  printf '\030'
    sleep 2;  printf '\030'
    sleep 2;  printf '\027'
    sleep 8;  printf '\017'
    sleep 6;  printf '\013'
    sleep 6;  printf '\004'
    sleep 3
) | qemu "$GUARDIAN_TIMEOUT"
STATUS=$?
check_status "$GUARDIAN_TIMEOUT" panic-allowed
check \
    "Test process fault (Ctrl-F)" \
    "[OOPS] Store page fault in process console" \
    "[AI] Process crash contained: console" \
    "[AI] Diagnosis: Bad pointer" \
    "[AI] Action: restart console" \
    "Process console restarted as pid 7, restart #1 (AI verdict)" \
    "Test driver fault (Ctrl-X)" \
    "inside driver faulty0: stopping only this process" \
    "[AI] Action: disable driver faulty0" \
    "Driver faulty0 disabled (AI verdict)" \
    "Process console restarted as pid 8, restart #2 (AI verdict)" \
    "faulty0 is disabled, nothing happened" \
    "Test freeze (Ctrl-W)" \
    "[AI] Kernel freeze detected: FREEZE" \
    "[AI] Core 0 stopped" \
    "[AI] Kernel code check: intact" \
    "[AI] Black box saved (crash #1, 1 in a row)" \
    "[AI] Action: warm kernel restart (only core 0" \
    "Warm restart #1 by the AI space" \
    "Device disabled by the AI space: faulty0" \
    "Previous crash detected: #1 FREEZE" \
    "Test code corruption (Ctrl-O)" \
    "[TRAP] Illegal instruction" \
    "bytes differ from the clean copy (code corrupted)" \
    "[AI] Diagnosis: Kernel code was overwritten" \
    "[AI] Black box saved (crash #2, 2 in a row)" \
    "Warm restart #2 by the AI space" \
    "Previous crash detected: #2 TRAP" \
    "Test kernel fault (Ctrl-K)" \
    "[AI] Kernel crash detected: TRAP" \
    "[AI] Black box saved (crash #3, 3 in a row)" \
    "[AI] Action: warm kernel restart into safe mode" \
    "Warm restart #3 by the AI space" \
    "Previous crash detected: #3 TRAP" \
    "SAFE MODE" \
    "Powering off"

if grep -qE ".\[AI\] |\[AI\] .*\[(INFO|WARN|OOPS|TRAP)\]" "$LOG"; then
    echo "  FAIL AI space and kernel output mixed on one line (UART lock)"
    FAILED=1
else
    echo "  ok   AI space and kernel lines never mixed (UART lock)"
fi

if grep -q "Power reboot\|\[AI\] Action: reboot" "$LOG"; then
    echo "  FAIL the AI space rebooted the machine instead of a warm restart"
    FAILED=1
fi

show_log_on_failure

echo "Run 4: a 4th crash in a row halts the kernel, the AI space stays online"
(
    sleep 4; printf '\020'
    sleep 8
) | qemu "$HALT_TIMEOUT"
STATUS=$?
check_status halt-expected panic-allowed
check \
    "Black box: no new crashes, total recorded: 3" \
    "SAFE MODE" \
    "Test panic (Ctrl-P)" \
    "[AI] Kernel crash detected: PANIC - Test panic (Ctrl-P)" \
    "[AI] Black box saved (crash #4, 4 in a row)" \
    "[AI] Action: halt the kernel (crash loop detected)" \
    "[AI] The AI space stays online"
show_log_on_failure

echo "RESULT: PASS"
