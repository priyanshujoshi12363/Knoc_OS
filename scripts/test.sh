#!/usr/bin/env bash
set -u

KERNEL=${KERNEL:-knocos.elf}
TIMEOUT=${TIMEOUT:-50}
GUARDIAN_TIMEOUT=${GUARDIAN_TIMEOUT:-60}
HALT_TIMEOUT=${HALT_TIMEOUT:-12}
LOG=$(mktemp)
DISK=$(mktemp)
trap 'rm -f "$LOG" "$DISK"' EXIT

FAILED=0

new_disk() {
    ./scripts/mkdisk.sh "$DISK" > /dev/null
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

# Types each argument into the shell as a command line ("^C" sends Ctrl-C),
# then powers off with Ctrl-D
boot() {
    (
        sleep 7
        for command in "$@"; do
            if [ "$command" = "^C" ]; then
                printf '\003'
            else
                printf '%s\r' "$command"
            fi
            sleep 0.6
        done
        sleep 1
        printf '\004'
        sleep 2
    ) | qemu "$TIMEOUT"
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

echo "Boot 1: self-tests and the shell"
boot "knocos-echo-test" "ls /bin" "cat /hello.txt" \
    "echo saved by the shell > /home/shell.txt" "cd /home" "pwd" "cat shell.txt" \
    "ps" "mem" "devices" "ai" "crashes" "counter" "^C" "kill 2" \
    "organize" "organize /home/Downloads --apply" "ls /home/Downloads/WhatsApp/Images" \
    "ls /home/Downloads/Random" "organize /home/Downloads --undo" "ls /home/Downloads"
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
    "KnocFS mounted on disk0" \
    "Wait queues verified" \
    "Multi-sector disk read verified" \
    "Scheduler started" \
    "AI-aware scheduling verified" \
    "Preemption verified" \
    "Interactive response verified" \
    "[hello] Hello from user mode!" \
    "[badcall] kernel pointer, unmapped pointer and unknown call were all refused" \
    "[SECURITY] noperm" \
    "[hog] 8 MiB allowed, 16 MiB more refused" \
    "[bigmem] AI agent got 256 MiB" \
    "[noperm] spawn and open were refused" \
    "[files] no note yet, writing /home/note.txt" \
    "[files] /hello.txt says: Hello from a file on KnocFS!" \
    "[files] /bin: badcall bigmem counter crash files hello hog knocsh modelcheck noperm organize spy" \
    "[files] 20000 bytes written across 5 blocks, read back, removed" \
    "[modelcheck] loaded 8 MiB model from /models/test-model.bin (1 extent)" \
    "User memory verified" \
    "Programs loaded from /bin on disk: 8" \
    "User mode verified" \
    "All self-tests passed" \
    "KnocOS shell (knocsh). Type help for commands." \
    "knocsh: unknown command: knocos-echo-test (type help)" \
    "  modelcheck" \
    "Hello from a file on KnocFS!" \
    "knoc:/home$ pwd" \
    "saved by the shell" \
    "knocsh        INTERACTIVE  RUNNING" \
    "RAM:  2048 MiB total" \
    "disk0     IRQ 1" \
    "AI space: online on core 1" \
    "No crashes recorded in the black box" \
    "[counter] 2" \
    "knocsh: counter stopped (Ctrl-C)" \
    "kill (only user programs can be stopped): permission denied" \
    "14 files: 10 by the AI, 3 by the rules, 1 to Random/" \
    "WhatsApp/Images/WhatsApp Image 2025-12-13 at 2.46.41 PM.jpeg" \
    "Disk Images/ubuntu-24.04-desktop-amd64.iso" \
    "Installers/code_1.93.1_amd64.deb" \
    "Spreadsheets/budget_2024.xlsx" \
    "random  Random/mystery.xyz" \
    "Files moved. Undo with: organize /home/Downloads --undo" \
    "       700  mystery.xyz" \
    "Restored 14 files and removed the empty folders" \
    "Powering off"
show_log_on_failure

echo "Boot 2: disk data and files survive a reboot"
boot "cat /home/shell.txt"
check \
    "Disk boot count: 2" \
    "[files] found my note from the last boot: KnocOS remembers this" \
    "knoc:/$ cat /home/shell.txt" \
    "Powering off"
show_log_on_failure

echo "Run 3: user programs, fault containment, driver disabling and warm kernel restarts"
new_disk
(
    sleep 6;  printf '\025'
    sleep 3;  printf '\005'
    sleep 2;  printf '\006'
    sleep 2;  printf '\030'
    sleep 2;  printf '\030'
    sleep 2;  printf '\027'
    sleep 8;  printf '\017'
    sleep 6;  printf '\013'
    sleep 7;  printf 'crashes\r'
    sleep 1;  printf 'ai\r'
    sleep 1;  printf '\004'
    sleep 3
) | qemu "$GUARDIAN_TIMEOUT"
STATUS=$?
check_status "$GUARDIAN_TIMEOUT" panic-allowed
check \
    "[OOPS] Store page fault in user program crash" \
    "[AI] Process crash contained: crash" \
    "[AI] Diagnosis: Null pointer: the program used an address near 0." \
    "Process crash restarted as pid" \
    "[AI] Action: leave crash stopped (it crashed 4 times)" \
    "Process crash left stopped: it keeps crashing" \
    "[SECURITY] spy" \
    "[OOPS] Load page fault in user program spy" \
    "last system calls: write, spawn, write  (forbidden: 1)" \
    "The page table blocked it." \
    "[AI] Security: it made 1 forbidden system call(s)" \
    "Process spy left stopped: suspicious" \
    "Test process fault (Ctrl-F)" \
    "[OOPS] Store page fault in process console" \
    "[AI] Process crash contained: console" \
    "[AI] Diagnosis: Bad pointer" \
    "[AI] Action: restart console" \
    "restart #1 (AI verdict)" \
    "Test driver fault (Ctrl-X)" \
    "inside driver faulty0: stopping only this process" \
    "[AI] Action: disable driver faulty0" \
    "Driver faulty0 disabled (AI verdict)" \
    "Process console restarted as pid" \
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
    "#3 TRAP in console" \
    "   AI action: warm kernel restart into safe mode" \
    "#2 TRAP in console" \
    "Warm kernel restarts: 3" \
    "Drivers disabled by the AI: faulty0" \
    "Safe mode: on" \
    "Powering off"

if grep -q "\[spy\] read kernel memory!" "$LOG"; then
    echo "  FAIL the spy program read kernel memory"
    FAILED=1
fi

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
