#!/usr/bin/env bash
set -u

KERNEL=${KERNEL:-knocos.elf}
TIMEOUT=${TIMEOUT:-180}
GUARDIAN_TIMEOUT=${GUARDIAN_TIMEOUT:-60}
HALT_TIMEOUT=${HALT_TIMEOUT:-12}
HEALTH_TIMEOUT=${HEALTH_TIMEOUT:-80}
LOG=$(mktemp)
DISK=$(mktemp)
LOG2=$(mktemp)
DISK2=$(mktemp)
WWW=$(mktemp -d)
WEB_PID=""
TLS_PID=""
trap 'rm -f "$LOG" "$DISK" "$LOG2" "$DISK2"; rm -rf "$WWW"; [ -n "$WEB_PID" ] && kill "$WEB_PID" 2>/dev/null; [ -n "$TLS_PID" ] && kill "$TLS_PID" 2>/dev/null' EXIT

FAILED=0

if ! ./scripts/get-busybox.sh > /dev/null; then
    echo "  MISS BusyBox could not be downloaded (Linux program tests need it)"
    FAILED=1
fi
if ! ./scripts/get-linux-base.sh > /dev/null; then
    echo "  MISS the Debian base could not be downloaded (dynamic Linux program tests need it)"
    FAILED=1
fi

new_disk() {
    ./scripts/mkdisk.sh "$DISK" > /dev/null
}

qemu() {
    timeout "$1" env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        qemu-system-riscv64 -machine virt -smp 8 -m 2G -bios none -nographic \
        -global virtio-mmio.force-legacy=false \
        -drive file="$DISK",if=none,format=raw,id=disk0 \
        -device virtio-blk-device,drive=disk0,bus=virtio-mmio-bus.0 \
        -netdev user,id=net0 -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.1 \
        -device virtio-rng-device,bus=virtio-mmio-bus.2 \
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
    "RAM 2048 MiB at 0x0000000080000000, 8 CPUs" \
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
    "AI space online (core 4, 256 MiB protected at 0x0000000090000000)" \
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
    "[files] /bin: agent ask badcall bigmem calc chat counter crash date diskload fetch files filler find healthd hello hog indexd index knocnetd knocnet knocsh leak libctest linuxtest modelcheck net noperm organized organize ping quiet recorder spawner spin spy tcc threadtest web busybox" \
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
    "AI space: online on core 4" \
    "No crashes recorded in the black box" \
    "[counter] 2" \
    "knocsh: counter stopped (Ctrl-C)" \
    "kill (only user programs can be stopped): permission denied" \
    "14 files: 10 by the AI, 3 by the rules, 1 to Random/" \
    "WhatsApp/Images/WhatsApp Image 2025-12-13 at 2.46.41 PM.jpeg" \
    "Disk Images/ubuntu-24.04-desktop-amd64.iso" \
    "Installers/code_1.93.1_amd64.deb" \
    "Spreadsheets/budget_2024.xlsx" \
    "random    Random/mystery.xyz" \
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
    "[AI]   decided by: the crash classifier NN (null_pointer" \
    "[AI]   decided by: the crash classifier NN (kernel_freeze" \
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
    "[AI] Kernel cores stopped" \
    "[AI] Kernel code check: intact" \
    "[AI] Black box saved (crash #1, 1 in a row)" \
    "[AI] Action: warm kernel restart (only the kernel cores" \
    "Warm restart #1 by the AI space" \
    "Device disabled by the AI space: faulty0" \
    "Previous crash detected: #1 FREEZE" \
    "Test code corruption (Ctrl-O)" \
    "[TRAP] Illegal instruction" \
    "bytes differ from the clean copy (code corrupted)" \
    "[AI] Diagnosis: Kernel code was overwritten" \
    "[AI]   decided by: rules (the kernel code differs from the clean copy)" \
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
    "[AI]   decided by: the crash classifier NN (kernel_panic" \
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
    "cat /home/todo.txt" "agent find todo" "agent write me a poem" "memory recent 6"
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

echo "Run 9: auto-organize sorts new downloads by itself and learns from a correction"
new_disk
boot "organize auto on" "sleep 30" "mkdir /home/College" \
    "move /home/Downloads/Documents/335505283.pdf /home/College" "organize learn" "organize personal" \
    "copy /home/College/335505283.pdf /home/Downloads/335505299.pdf" "echo buy milk > /home/Downloads/todo.txt" \
    "sleep 35" "ls /home/College" "organize auto off" "memory why /home/College/335505283.pdf" \
    "organize forget" "organize personal"
check \
    "Auto-organize is on" \
    "[ORGANIZE] 335505283.pdf -> Documents/335505283.pdf (ai)" \
    "[ORGANIZE] mystery.xyz -> Random/mystery.xyz (random)" \
    "[ORGANIZE] learned: 335505283.pdf belongs in /home/College" \
    "[ORGANIZE] personal model trained: 1 folder of yours" \
    "335505283.pdf  ->  /home/College" \
    "[ORGANIZE] 335505299.pdf -> /home/College/335505299.pdf (personal)" \
    "[ORGANIZE] todo.txt -> Text/todo.txt (ai)" \
    "knocsh: file /home/Downloads/Documents/335505283.pdf --moved_to--> file /home/College/335505283.pdf" \
    "organize: forgot everything it learned from you" \
    "Nothing learned yet"
show_log_on_failure

echo "Run 10: the crash classifier NN in the AI space diagnoses each kind of program crash"
new_disk
boot "crash stack" "sleep 1" "crash jump 5" "sleep 1" "crash misaligned 3" "sleep 1" "crash unmapped 7" "sleep 1" \
    "crash illegal 2" "sleep 1" "crash wild 4" "sleep 1"
check \
    "[AI] Diagnosis: Stack overflow: the stack grew past its end" \
    "[AI]   decided by: the crash classifier NN (stack_overflow" \
    "[AI] Diagnosis: Jump to a bad address" \
    "[AI]   decided by: the crash classifier NN (bad_jump" \
    "[AI]   decided by: the crash classifier NN (misaligned" \
    "[AI]   decided by: the crash classifier NN (unallocated" \
    "[AI]   decided by: the crash classifier NN (illegal_instruction" \
    "[AI]   decided by: the crash classifier NN (bad_pointer"
check_absent "decided by: rules (the crash classifier NN was unsure)"
show_log_on_failure

echo "Run 11: the context tracker and the live permission watch"
new_disk
boot "noperm repeat &" "sleep 5" "mkdir /home/code" "cd /home/code" "echo int main > main.c" "echo notes > todo.txt" \
    "cd /home/Downloads" "hello" "cd /home/code" "context" "health" "ask what was I working on?"
check \
    "[SECURITY] noperm keeps asking for things it has no permission for" \
    "[SECURITY] recovered: stopped noperm" \
    "healthd: program noperm --anomaly--> diagnosis repeated permission denials" \
    "Folders you work in: /home/code" \
    "Programs you use:" \
    "- Recently the user worked most in: /home/code"
show_log_on_failure

echo "Run 12: the C library: libctest checks it, and calc is a normal C program"
new_disk
boot "libctest alpha 42" "calc 2 ^ 10" "calc" "?7 / 2" "?sqrt 2" "?oops" "?quit"
check \
    "libctest: testing the KnocOS C library" \
    "checks passed, 0 failed" \
    "1024" \
    "= 3.5" \
    "= 1.41421" \
    "?  try 2 + 3 or sqrt 2"
check_absent "  FAIL "
show_log_on_failure

echo "Run 13: the C compiler inside KnocOS compiles and runs programs"
new_disk
python3 tools/knocfs.py put-many "$DISK" /home/code scripts/fixtures/prog.c scripts/fixtures/util.c \
    scripts/fixtures/main2.c scripts/fixtures/bad.c scripts/fixtures/threads.c
boot "cd /home/code" "tcc -v" "tcc prog.c -o prog" "./prog 20" "tcc main2.c util.c -o two" "./two" \
    "tcc threads.c -o threads" "./threads" \
    "tcc bad.c -o bad" 'echo compile status $?' \
    'agent --call {"name": "run_app", "arguments": {"app": "tcc", "args": "bad.c -o bad"}}' "?y"
check \
    "tcc version 0.9.28rc" \
    "fibonacci(20) = 6765" \
    "closest first: near mid far" \
    "file says: compiled inside KnocOS" \
    "two files: square(12) = 144" \
    "threads built by tcc: total 2501500, joined 1000 and 2000" \
    "bad.c:6: error: ';' expected (got 'return')" \
    "compile status 1"
show_log_on_failure

echo "Run 14: networking: ping, DNS-free downloads from a web server on the host, errors"
new_disk
echo "hello from the host web server" > "$WWW/hello.txt"
head -c 200000 /dev/urandom > "$WWW/big.bin"
PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
(cd "$WWW" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 > /dev/null 2>&1) &
WEB_PID=$!
sleep 1
boot "net" "ping 10.0.2.2 2" "fetch http://10.0.2.2:$PORT/hello.txt /home/hello.txt" "cat /home/hello.txt" \
    "fetch http://10.0.2.2:$PORT/big.bin /home/big.bin" "fetch http://10.0.2.2:$PORT/missing.txt" \
    "fetch http://10.0.2.2:1/x" "net"
check \
    "net0: up" \
    "address    10.0.2.15" \
    "reply from 10.0.2.2: seq=2" \
    "fetch: 200 OK, 31 bytes saved to /home/hello.txt" \
    "hello from the host web server" \
    "fetch: 200 OK, 200000 bytes saved to /home/big.bin" \
    "fetch: the server answered 404" \
    "fetch: cannot connect to 10.0.2.2:1 (refused)" \
    "0 open connections"
if python3 tools/knocfs.py cat "$DISK" /home/big.bin | cmp -s - "$WWW/big.bin"; then
    echo "  ok   the 200000 byte download is identical to the original"
else
    echo "  MISS the downloaded file differs from the original"
    FAILED=1
fi
show_log_on_failure

echo "Run 15: HTTPS with certificate checks, the real-time clock and the text browser"
new_disk
cp scripts/fixtures/index.html scripts/fixtures/page2.html "$WWW/"
CERTS=$(mktemp -d)
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$CERTS/ca.key" -out "$CERTS/ca.pem" -days 30 \
    -subj "/CN=KnocOS Test CA" -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign" 2> /dev/null
openssl req -newkey rsa:2048 -nodes -keyout "$CERTS/server.key" -out "$CERTS/server.csr" \
    -subj "/CN=knoc.test" 2> /dev/null
printf 'subjectAltName=DNS:knoc.test\nbasicConstraints=CA:FALSE\n' > "$CERTS/ext.txt"
openssl x509 -req -in "$CERTS/server.csr" -CA "$CERTS/ca.pem" -CAkey "$CERTS/ca.key" -CAcreateserial \
    -out "$CERTS/server.pem" -days 30 -extfile "$CERTS/ext.txt" 2> /dev/null
python3 tools/knocfs.py cat "$DISK" /etc/ssl/certs.pem > "$CERTS/bundle.pem" 2> /dev/null || true
cat "$CERTS/ca.pem" >> "$CERTS/bundle.pem"
python3 tools/knocfs.py put "$DISK" "$CERTS/bundle.pem" /etc/ssl/certs.pem
python3 tools/knocfs.py put-text "$DISK" /etc/hosts "10.0.2.2 host knoc.test
"
TLS_PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
python3 -c '
import http.server, os, ssl, sys
os.chdir(sys.argv[1])
server = http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), http.server.SimpleHTTPRequestHandler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(sys.argv[3], sys.argv[4])
server.socket = context.wrap_socket(server.socket, server_side=True)
server.serve_forever()
' "$WWW" "$TLS_PORT" "$CERTS/server.pem" "$CERTS/server.key" > /dev/null 2>&1 &
TLS_PID=$!
sleep 1
boot "date" "fetch https://knoc.test:$TLS_PORT/hello.txt /home/secure.txt" "cat /home/secure.txt" \
    "fetch https://10.0.2.2:$TLS_PORT/hello.txt" "fetch https://knoc.test:$PORT/hello.txt" \
    "web -dump http://host:$PORT/index.html" \
    "web https://knoc.test:$TLS_PORT/" "?1" "?u" "?q" 'echo web status $?'
check \
    " UTC 20" \
    "fetch: 200 OK, 31 bytes saved to /home/secure.txt" \
    "hello from the host web server" \
    "fetch: the certificate belongs to a different name" \
    "fetch: the server didn't answer with https (TLS); try http://" \
    "KnocOS test page" \
    "# Welcome to KnocOS" \
    "Fish & chips cost <5> €, café ✓" \
    "  * first item" \
    "  * second item with Second page[1]" \
    "  code   stays" \
    "  as     typed" \
    "Plain text[2] and absolute[3]" \
    "[1] http://host:$PORT/page2.html" \
    "[2] http://host:$PORT/hello.txt" \
    "[3] https://knoc.test/abs" \
    "web: loading https://knoc.test:$TLS_PORT/page2.html" \
    "This is page two, reached by following a link." \
    "web status 0"
check_absent "SCRIPT_SHOULD_NOT_SHOW" "COMMENT_SHOULD_NOT_SHOW" "color: red"
kill "$WEB_PID" "$TLS_PID" 2>/dev/null
WEB_PID=""
TLS_PID=""
rm -rf "$CERTS"
show_log_on_failure

echo "Run 16: 8 cores: parallel programs, AI cores, threads, and a warm restart while every core is busy"
new_disk
(
    sleep 6;  printf 'run spin &\r'
    sleep 1;  printf 'run spin &\r'
    sleep 1;  printf 'run spin &\r'
    sleep 3;  printf 'cpus\r'
    sleep 3;  printf 'ps\r'
    sleep 2;  printf 'threadtest\r'
    sleep 12; printf '\013'
    sleep 12; printf 'cpus\r'
    sleep 3;  printf '\004'
    sleep 3
) | qemu "$GUARDIAN_TIMEOUT"
STATUS=$?
check_status "$GUARDIAN_TIMEOUT" panic-allowed
check \
    "CPU cores online: 7 kernel cores (4 general: 0-3, 3 AI: 5-7) + the AI space on core 4" \
    "4     AI space  -     guardian: crash diagnosis, black box, warm restart" \
    "threadtest: 3 threads summed the work: same answer as 1 thread" \
    "threadtest: mutex counter 60000 (expected 60000)" \
    "threadtest: malloc from 3 threads at once: ok" \
    "[AI] Kernel cores stopped" \
    "Warm restart #1 by the AI space"
SPIN_CORES=$(tr -d '\r' < "$LOG" | grep -E "^ +[0-9]+ +spin +NORMAL +RUNNING" | awk '{print $NF}' | sort -u | grep -cE "^[0-3]$")
AI_ON_AI_CORE=$(tr -d '\r' < "$LOG" | grep -cE "^ +[0-9]+ +healthd +BACKGROUND .* user +[567]$")
ONLINE_TWICE=$(grep -c "CPU cores online: 7 kernel cores" "$LOG")
if [ "$SPIN_CORES" -ge 3 ]; then
    echo "  ok   3 spin programs ran at the same time on 3 different general cores"
else
    echo "  MISS 3 spin programs on 3 different general cores (found $SPIN_CORES)"
    FAILED=1
fi
if [ "$AI_ON_AI_CORE" -ge 1 ]; then
    echo "  ok   the AI health monitor runs on an AI core (5-7)"
else
    echo "  MISS the AI health monitor runs on an AI core"
    FAILED=1
fi
if [ "$ONLINE_TWICE" -ge 2 ]; then
    echo "  ok   all 7 kernel cores came back after the warm restart"
else
    echo "  MISS all kernel cores came back after the warm restart"
    FAILED=1
fi
show_log_on_failure

echo "Run 17: KnocNet: two KnocOS machines pair, then talk over an encrypted link"
new_disk
./scripts/mkdisk.sh "$DISK2" > /dev/null
python3 tools/knocfs.py mkdir "$DISK2" /home/Shared
python3 tools/knocfs.py put-text "$DISK2" /home/Shared/shared.txt "a file shared by beta"
KN_PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
QEMU_NETDEV_EXTRA=",hostfwd=tcp:127.0.0.1:$KN_PORT-:7000" COMMAND_WAIT=150 \
    python3 scripts/drive.py "$((TIMEOUT + 120))" "$LOG2" "$DISK2" "$KERNEL" \
    "knocnet name beta" "knocnet pair wait 424242" "sleep 40" "ls /home/KnocNet/alpha" \
    "cat /home/KnocNet/alpha/note.txt" "knocnet peers" "knocnet unpair alpha" "sleep 100" &
BETA_PID=$!
sleep 15
COMMAND_WAIT=90 boot "knocnet selftest" "knocnet name alpha" "knocnet ping 10.0.2.2:$KN_PORT" \
    "knocnet pair 10.0.2.2:$KN_PORT 111111" "knocnet pair 10.0.2.2:$KN_PORT 424242" "knocnet peers" \
    "knocnet ping beta" "knocnet status beta" "echo hello over knocnet > /home/note.txt" \
    "knocnet send beta /home/note.txt" "knocnet get beta /home/Shared/shared.txt /home/got.txt" "cat /home/got.txt" \
    "knocnet get beta /etc/hosts" "knocnet ask beta what is 2+2" "sleep 45" "knocnet ping beta"
wait "$BETA_PID"
BETA_STATUS=$?
check \
    "key exchange (X25519): both sides agree" \
    "a changed signature is rejected" \
    "a changed message is rejected" \
    "refused: that machine doesn't trust this one (pair first)" \
    "knocnet: pairing failed: wrong pairing code" \
    "paired with beta (" \
    "beta answered over an encrypted link" \
    "beta: up " \
    "SAVED /home/KnocNet/alpha/note.txt 19" \
    "got /home/Shared/shared.txt from beta: 21 bytes saved to /home/got.txt" \
    "a file shared by beta" \
    "ERROR only files in /home/Shared/ can be fetched" \
    "ask: cannot load /models/qwen.kllm"
if [ "$(grep -c "refused: that machine doesn't trust this one" "$LOG")" -ge 2 ]; then
    echo "  ok   after beta unpaired alpha, alpha is refused again"
else
    echo "  MISS after beta unpaired alpha, alpha is refused again"
    FAILED=1
fi
for line in "[KNOCNET] ready: " "[KNOCNET] refused an unknown machine" "[KNOCNET] paired with alpha" \
    "[KNOCNET] alpha sent note.txt (19 bytes) -> /home/KnocNet/alpha/note.txt" "hello over knocnet" \
    "[KNOCNET] alpha fetched /home/Shared/shared.txt (21 bytes)" \
    "[KNOCNET] alpha asked for /etc/hosts: refused" "alpha is no longer trusted"; do
    if grep -qF -- "$line" "$LOG2"; then
        echo "  ok   beta: $line"
    else
        echo "  MISS beta: $line"
        FAILED=1
    fi
done
if [ "$BETA_STATUS" -ne 0 ]; then
    echo "  FAIL the second machine (beta) did not power off cleanly (status $BETA_STATUS)"
    FAILED=1
fi
if [ "$FAILED" -ne 0 ]; then
    echo "----- beta -----"
    cat "$LOG2"
fi
show_log_on_failure

echo "Run 18: search by meaning: find files by what they are about, with dates and types"
new_disk
python3 tools/knocfs.py mkdir "$DISK" /home/Documents /home/recipes /home/code /home/Photos /home/work
python3 tools/knocfs.py put "$DISK" scripts/fixtures/search/march_statement.txt /home/Documents/march_statement.txt
python3 tools/knocfs.py put "$DISK" scripts/fixtures/search/old_statement.txt /home/Documents/old_statement.txt
python3 tools/knocfs.py put "$DISK" scripts/fixtures/search/pasta.txt /home/recipes/pasta.txt
python3 tools/knocfs.py put "$DISK" scripts/fixtures/search/fib.c /home/code/fib.c
python3 tools/knocfs.py put "$DISK" scripts/fixtures/search/profile_2026.txt /home/work/profile_2026.txt
python3 tools/knocfs.py put "$DISK" scripts/fixtures/search/sync_notes.md /home/work/sync_notes.md
python3 tools/knocfs.py put "$DISK" scripts/fixtures/search/goa_beach_trip.jpg /home/Photos/goa_beach_trip.jpg
LAST_MONTH=$(python3 -c 'import datetime as d; t = d.datetime.now(d.timezone.utc).replace(day=1) - d.timedelta(days=10); print(int(t.timestamp()))')
python3 tools/knocfs.py touch "$DISK" /home/Documents/march_statement.txt "$LAST_MONTH"
python3 tools/knocfs.py touch "$DISK" /home/Documents/old_statement.txt 1719800000
boot "index status" "find invoice" "find cooking" "find program source" "find cv" "find holiday pictures" \
    "find meeting minutes" "find invoice from last month" "find bill 2024" "find photos" \
    "echo my passport number is K1234567 > /home/travel_papers.txt" "find passport" "index status" \
    'agent find something about pasta' "organize /home/Downloads --apply" "ask where is my spreadsheet about money" "sleep 25"
check \
    "organize moved budget_2024.xlsx from /home/Downloads to /home/Downloads/Spreadsheets" \
    "[INDEX] ready: " \
    "find: image files" \
    "find: from last month" \
    "  /home/Documents/old_statement.txt  2024-07-01"
FIRST_HIT() { tr -d '\r' < "$LOG" | awk -v q="knoc:/$ find $1" '$0 == q {f = 1; next} f && /%/ {print; exit} f && /^knoc:/ {exit}'; }
for pair in "invoice|march_statement.txt" "cooking|pasta.txt" "program source|fib.c" "cv|profile_2026.txt" \
    "holiday pictures|goa_beach_trip.jpg" "meeting minutes|sync_notes.md" "invoice from last month|march_statement.txt" \
    "passport|travel_papers.txt"; do
    QUERY=${pair%%|*}
    WANT=${pair##*|}
    if FIRST_HIT "$QUERY" | grep -qF "$WANT"; then
        echo "  ok   find $QUERY -> $WANT first"
    else
        echo "  MISS find $QUERY -> $WANT first (got: $(FIRST_HIT "$QUERY"))"
        FAILED=1
    fi
done
if tr -d '\r' < "$LOG" | awk '/knoc:\/\$ find invoice from last month/,/knoc:\/\$ find bill 2024/' | grep -q old_statement; then
    echo "  MISS last month must leave out the 2024 bill"
    FAILED=1
else
    echo "  ok   last month leaves out the 2024 bill"
fi
if tr -d '\r' < "$LOG" | awk '/knoc:\/\$ find photos/,/knoc:\/\$ echo my passport/' | grep -E "^ +[0-9]+%" | grep -vq goa_beach_trip; then
    echo "  MISS photos shows only pictures"
    FAILED=1
else
    echo "  ok   photos shows only pictures"
fi
if tr -d '\r' < "$LOG" | awk '/agent find something about pasta/,0' | grep -qF "/home/recipes/pasta.txt"; then
    echo "  ok   the agent finds files by meaning"
else
    echo "  MISS the agent finds files by meaning"
    FAILED=1
fi
show_log_on_failure

echo "Run 19: Linux programs: a test program, BusyBox tools, vi, and a crash contained by the AI"
new_disk
printf 'pear\napple pie\nbanana\napple juice\ncherry\n' > "$WWW/data.txt"
python3 tools/knocfs.py put "$DISK" "$WWW/data.txt" /home/data.txt
DATA_MD5=$(md5sum "$WWW/data.txt" | cut -d' ' -f1)
boot "linuxtest hello" 'echo linux exit $?' "busybox uname -m" "busybox wc -l /home/data.txt" \
    "busybox md5sum /home/data.txt" "busybox sed s/apple/APPLE/ /home/data.txt" "busybox sort /home/data.txt" \
    "grep -c apple /home/data.txt" "busybox tar -cf /tmp/d.tar /home/data.txt" "busybox tar -tf /tmp/d.tar" \
    "busybox gzip -c /home/data.txt > /tmp/d.gz" "busybox gunzip -c /tmp/d.gz" "busybox df" "busybox ps" \
    "linuxtest crash" "sleep 4"
check \
    "linuxtest: hello from a Linux program, argc 2" \
    "linuxtest: argv[1] = hello" \
    "linuxtest: environment has PATH=/bin" \
    "linuxtest: uname says Linux riscv64" \
    "linuxtest: read 28 bytes from /hello.txt" \
    "linuxtest: write, read back and lseek ok" \
    "linuxtest: stat size 17, a regular file" \
    "linuxtest: missing file gives -2" \
    "linuxtest: brk grew the heap by 100000 bytes" \
    "linuxtest: mmap gave 1048576 zeroed bytes" \
    "linuxtest: the clock says year 20" \
    "linuxtest: getrandom gave 16 random bytes" \
    "linuxtest: getdents64 found /bin" \
    "linuxtest: mkdirat and unlinkat ok" \
    "linuxtest: an unknown system call gives -38" \
    "linux exit 3" \
    "riscv64" \
    "5 /home/data.txt" \
    "$DATA_MD5  /home/data.txt" \
    "APPLE pie" \
    "home/data.txt" \
    "knocfs " \
    "busybox" \
    "[OOPS] Store page fault in user program linuxtest" \
    "[AI] Diagnosis: Null pointer"
if tr -d '\r' < "$LOG" | awk '/knoc:\/\$ busybox sort/,/knoc:\/\$ grep -c apple/' | tr '\n' ' ' | \
    grep -q "apple juice apple pie banana cherry pear"; then
    echo "  ok   sort gives the lines in order"
else
    echo "  MISS sort gives the lines in order"
    FAILED=1
fi
if tr -d '\r' < "$LOG" | awk '/knoc:\/\$ grep -c apple/ {getline; print; exit}' | grep -qx "2"; then
    echo "  ok   grep runs from BusyBox without typing busybox"
else
    echo "  MISS grep runs from BusyBox without typing busybox"
    FAILED=1
fi
if tr -d '\r' < "$LOG" | awk '/knoc:\/\$ busybox gunzip/,/knoc:\/\$ busybox df/' | grep -q "apple juice"; then
    echo "  ok   gzip and gunzip give the file back"
else
    echo "  MISS gzip and gunzip give the file back"
    FAILED=1
fi
show_log_on_failure

new_disk
(
    sleep 8;  printf 'busybox vi /tmp/edit.txt\r'
    sleep 5;  printf 'ihello from vi on KnocOS'
    sleep 2;  printf '\033'
    sleep 2;  printf ':wq\r'
    sleep 4;  printf 'cat /tmp/edit.txt\r'
    sleep 2;  printf '\004'
    sleep 3
) | qemu "$TIMEOUT"
STATUS=$?
check_status "$TIMEOUT" no-panic
check "'/tmp/edit.txt' 1L, 24C"
if tr -d '\r' < "$LOG" | grep -qx "hello from vi on KnocOS"; then
    echo "  ok   vi edited and saved a file"
else
    echo "  MISS vi edited and saved a file"
    FAILED=1
fi
show_log_on_failure

echo "Run 20: dynamic Linux programs: glibc's loader and libraries, bash, Lua, symlinks, mprotect"
new_disk
python3 - "$WWW/broken-lua" <<'PY'
import sys
data = open("build/linux/root/usr/bin/lua5.4", "rb").read()
open(sys.argv[1], "wb").write(data.replace(b"libreadline.so.8", b"libmissingx.so.8"))
PY
python3 tools/knocfs.py put "$DISK" "$WWW/broken-lua" /home/broken-lua
HELLO_SHA=$(printf 'Hello from a file on KnocFS!' | sha256sum | cut -d' ' -f1)
boot "lua5.4 -e 'print(2^10)'" "lua5.4 -e 'local t={} for i=1,1000 do t[i]=i*i end print(#t, t[1000])'" \
    "/lib/ld-linux-riscv64-lp64d.so.1 --list /usr/bin/lua5.4" "bash --version" \
    "bash -c 'echo \$((6*7)); for i in 1 2 3; do echo n\$i; done'" "/usr/bin/busybox echo dynamic busybox" \
    "/usr/bin/busybox sha256sum /hello.txt" "busybox ln -s /hello.txt /home/link.txt" "cat /home/link.txt" \
    "busybox readlink /home/link.txt" "busybox ls -l /home/link.txt" "/home/broken-lua -v" "linuxtest" \
    "linuxtest protect" "sleep 4"
check \
    "1024.0" \
    "1000	1000000" \
    "libc.so.6 => /usr/lib/riscv64-linux-gnu/libc.so.6" \
    "libreadline.so.8 => /usr/lib/riscv64-linux-gnu/libreadline.so.8" \
    "GNU bash, version 5.3" \
    "n3" \
    "dynamic busybox" \
    "$HELLO_SHA  /hello.txt" \
    "Hello from a file on KnocFS!" \
    "/home/link.txt -> /hello.txt" \
    "error while loading shared libraries: libmissingx.so.8: cannot open shared object file" \
    "exited with code 127" \
    "linuxtest: mprotect and munmap ok" \
    "linuxtest: MAP_FIXED replaced a page with a fresh one" \
    "linuxtest: writing to a read-only page" \
    "[OOPS] Store page fault in user program linuxtest"
if tr -d '\r' < "$LOG" | grep -qx "42"; then
    echo "  ok   bash computed 6*7"
else
    echo "  MISS bash computed 6*7"
    FAILED=1
fi
if tr -d '\r' < "$LOG" | grep -qx "/hello.txt"; then
    echo "  ok   readlink shows the link target"
else
    echo "  MISS readlink shows the link target"
    FAILED=1
fi
show_log_on_failure

echo "RESULT: PASS"
