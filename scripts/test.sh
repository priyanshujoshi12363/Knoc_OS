#!/usr/bin/env bash
set -u

KERNEL=${KERNEL:-knocos.elf}
TIMEOUT=${TIMEOUT:-180}
GUARDIAN_TIMEOUT=${GUARDIAN_TIMEOUT:-60}
HALT_TIMEOUT=${HALT_TIMEOUT:-12}
HEALTH_TIMEOUT=${HEALTH_TIMEOUT:-80}
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

boot() {
    python3 scripts/drive.py "$TIMEOUT" "$LOG" "$DISK" "$KERNEL" "$@"
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
        if grep -qF -- "$line" "$LOG"; then
            echo "  ok   $line"
        else
            echo "  MISS $line"
            FAILED=1
        fi
    done
}

check_absent() {
    for line in "$@"; do
        if grep -qF -- "$line" "$LOG"; then
            echo "  FAIL unexpected: $(grep -F -- "$line" "$LOG" | head -1)"
            FAILED=1
        else
            echo "  ok   no $line"
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
    "ls /home/Downloads/Random" "organize /home/Downloads --undo" "ls /home/Downloads" \
    "memory" "memory why /home/Downloads/Documents/335505283.pdf" "memory recent 3" \
    "ask why was 335505283.pdf moved?"
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
    "[files] /bin: agent ask badcall bigmem chat counter crash diskload files filler healthd hello hog knocsh leak modelcheck noperm organize quiet recorder spawner spin spy" \
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
    "Memory graph ready: 0 nodes, 0 links (boot 1)" \
    "was moved here by organize from /home/Downloads/335505283.pdf, because:" \
    "organize: file /home/Downloads/335505283.pdf --classified_as--> type document" \
    "organize: file /home/Downloads/335505283.pdf --came_from--> source other" \
    "--restored_to--> file /home/Downloads/335505283.pdf" \
    "[HEALTH] anomaly detector running" \
    "[ask] facts from the memory graph and the system:" \
    "organize moved 335505283.pdf from /home/Downloads to /home/Downloads/Documents because it is a document file" \
    "ask: cannot load /models/qwen.kllm" \
    "Powering off"
check_absent "[HEALTH] memory leak in" "[HEALTH] CPU hog in" "[HEALTH] disk thrashing in" \
    "[HEALTH] spawn storm in" "[HEALTH] disk filling up in"
show_log_on_failure

echo "Boot 2: disk data and files survive a reboot"
boot "cat /home/shell.txt" "memory find WhatsApp" "memory show organize"
check \
    "Disk boot count: 2" \
    "[files] found my note from the last boot: KnocOS remembers this" \
    "knoc:/$ cat /home/shell.txt" \
    "links (boot 2)" \
    "file       /home/Downloads/WhatsApp/Images/WhatsApp Image 2025-12-13 at 2.46.41 PM.jpeg" \
    "actor knocsh --started--> program organize" \
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
    sleep 1;  printf 'memory show faulty0\r'
    sleep 1;  printf 'memory find crash\r'
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
    "ai-space: actor ai-space --disabled--> driver faulty0" \
    "crash      crash #3" \
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

echo "Run 5: the anomaly detector finds problems, also two at once, and fixes them"
new_disk
(
    sleep 8;  printf 'leak 2048 &\r'
    sleep 18; printf 'spin &\r'
    sleep 1;  printf 'spawner &\r'
    sleep 16; printf 'kill spin\r'
    sleep 14; printf 'health\r'
    sleep 1;  printf '\004'
    sleep 3
) | qemu "$HEALTH_TIMEOUT"
STATUS=$?
check_status "$HEALTH_TIMEOUT" no-panic
check \
    "[HEALTH] memory leak in leak (+" \
    "[HEALTH] recovered: stopped leak" \
    "[HEALTH] CPU hog in spin (" \
    "[HEALTH] recovered: moved to background priority: spin" \
    "[HEALTH] spawn storm in spawner" \
    "[HEALTH] recovered: stopped spawner" \
    "[HEALTH] CPU hog in spin is over" \
    "healthd: actor healthd --stopped--> program leak" \
    "healthd: actor healthd --lowered--> program spin" \
    "healthd: program spawner --anomaly--> diagnosis spawn storm"
check_absent "disk thrashing in" "disk filling up in" "CPU hog in unknown"
show_log_on_failure

echo "Run 6: the agent uses tools and apps, and asks before it changes anything"
new_disk
boot "agent --tools" "agent sort my downloads" "?n" "agent sort my downloads" "?y" \
    "spin &" "agent stop spin" "?y" "agent what is wrong" \
    'agent --call {"name": "list_folder", "arguments": {"path": "/home"}}' \
    'agent --call {"name": "write_file", "arguments": {"path": "/bin/evil", "text": "x"}}' \
    'agent --call {"name": "write_file", "arguments": {"path": "/home/todo.txt", "text": "buy milk"}}' "?y" \
    "cat /home/todo.txt" "agent find todo" "agent write me a poem" "memory recent 4"
check \
    "organize  asks first: Sorts the files of a folder" \
    "[agent] skipped" \
    "Files moved. Undo with: organize /home/Downloads --undo" \
    "[agent] stop_program(name=spin) Allow? (y/n) y" \
    "stopped spin" \
    "Running programs:" \
    "Downloads/" \
    "write_file(path=/bin/evil, text=x): error: the agent may only change files inside /home and /tmp" \
    "wrote 8 bytes to /home/todo.txt" \
    "buy milk" \
    "[agent] no language model" \
    "agent: actor agent --stopped--> program spin" \
    "agent: actor agent --action--> action write_file /home/todo.txt buy milk"
check_absent "/bin/evil, text=x) Allow?"
show_log_on_failure

echo "Run 7: shell scripts, redirection, the startup script and the agent running a script"
new_disk
python3 tools/knocfs.py put "$DISK" scripts/fixtures/test.ksh /home/test.ksh
python3 tools/knocfs.py put "$DISK" scripts/fixtures/startup.ksh /etc/startup.ksh
boot "run /home/test.ksh apple" 'echo exit code $?' "ps > /tmp/ps.txt" "cat /tmp/ps.txt" "if 1 == 1" \
    'agent --call {"name": "run_script", "arguments": {"path": "/home/test.ksh", "args": "pear"}}' "?y" \
    'agent --call {"name": "run_script", "arguments": {"path": "/home/nope.ksh"}}' \
    'agent --call {"name": "run_script", "arguments": {"path": "/home/test.ksh"}}' "?n"
check \
    "startup script ran" \
    "hello from KnocOS, 1 arguments, first apple" \
    "downloads found" \
    "first is apple" \
    "item kiwi" \
    "round 3" \
    "nothing is not there" \
    "status after a failure: 1" \
    "[hello] Hello from user mode!" \
    "a program can be a condition" \
    "exit code 3" \
    "knocsh        INTERACTIVE" \
    "knocsh: if works inside scripts" \
    "----- /home/test.ksh -----" \
    "first is not apple" \
    "error: path must be an existing .ksh script" \
    "[agent] skipped"
check_absent "never runs: KnocOS" "script line"
show_log_on_failure

echo "Run 8: chat commands work, and without a model it says so and keeps running"
new_disk
boot "chat" "?/help" "?hello there" "?/tools" "?/bogus" "?/save /home/chat.txt" "?/exit" \
    "agent stop nothing" "agent lower knocsh"
check \
    "KnocOS chat. Type a message, /help for commands, /exit to leave." \
    "/new          start a new conversation" \
    "knocos: no language model" \
    "run_script(path,args)  asks first" \
    "chat: unknown command, type /help" \
    "chat: saved 0 bytes to /home/chat.txt" \
    "chat: bye" \
    "stop_program(name=nothing): error: no running program with that name" \
    "lower_priority(name=knocsh): error: that program is protected"
check_absent "name=nothing) Allow?" "name=knocsh) Allow?"
show_log_on_failure

echo "RESULT: PASS"
