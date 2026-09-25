# Changelog

All notable changes to KnocOS are listed here. Versions follow [Semantic Versioning](https://semver.org/): while KnocOS is below `1.0.0`, every minor version is a development milestone.

## [0.20.0] - 2026-09-26

Chat: talk to KnocOS, and it remembers the conversation.

### Added
- `/bin/chat`: a conversation with the language model that stays loaded. Every new message only adds its own tokens to the model's memory (the KV cache), so earlier messages are never read again
  - Each message gets facts from the memory graph and the system when it needs them (like `ask`)
  - The agent's tools and apps work inside the chat, with the same y/n before every change
  - `/help`, `/new` (a new conversation), `/save FILE` (the transcript), `/tools`, `/exit`
  - When the 2048-token memory is full, the chat says so and starts a new conversation
  - The model loads on the first message, so the commands work even without a model
- `user/assist.c` (`assist.h`): the tool system, safety gate, rules and model loop shared by `agent` and `chat`; one system prompt and one prompt cache (`/tmp/assistant-prefix.kv`) for both
- Agent rules: "lower NAME" / "slow down NAME" → `lower_priority`; a path anywhere in the message ("what files are in /home/Downloads?") → `list_folder` for a folder, `read_file` for a file
- Grounding: before the model answers, chat (and the agent's model path) runs the read-only tools the rules find and gives the model their real results, so even a small model answers from real data instead of guessing
- `make test` Run 8: chat commands, `/save`, the no-model message, and refusals before y/n

### Changed
- The agent refuses to stop or lower a program that isn't running or is protected before it asks y/n (like the path checks)
- The agent also reads memory-graph facts when the model plans (it prints how many)
- The test driver answers chat prompts (`you: `) the same way as y/n questions

## [0.19.0] - 2026-09-26

Shell scripts: you and the agent can automate tasks.

### Added
- knocsh runs scripts (`run FILE.ksh [ARGS]`, or a path / `.ksh` name as the command) and `knocsh FILE [ARGS]` runs one without a prompt
  - Variables: `set NAME VALUE`, `$NAME`, `inc NAME [N]`, `set` alone lists them; `\$` for a plain dollar sign
  - Arguments: `$1` to `$9`, `$#`; the last exit code: `$?`
  - `if COND` / `else` / `end`, `for X in A B C` / `end`, `while COND` / `end`, `exit N`, `# comments`
  - Conditions: `exists PATH`, `A == B`, `A != B`, `not COND`, or any command (true when it exits with 0)
  - Safety: a `while` loop stops after 10,000 rounds, scripts nest at most 4 deep, and Ctrl-C on a program stops the whole script
- `COMMAND > FILE` and `COMMAND >> FILE` for every built-in command and program
- `copy FROM TO` (`cp`) and `move FROM TO` (`mv`); a folder as the target keeps the file name
- `/etc/startup.ksh` runs when the shell starts
- Built-in commands, unknown commands and programs set the exit code
- Agent tool `run_script(path, args)`: shows the whole script, asks y/n, runs it with `knocsh FILE` and reads its output
- Quiet output capture (`spawn_capture(name, args, quiet)`): the output goes only to the program that asked for it; the user library can switch where `print` writes (`set_output`)
- `make test` Run 7: every script feature, redirection, the startup script and the agent running a script

### Changed
- A shell started with a script file doesn't take over the terminal

## [0.18.0] - 2026-09-25

The agent: the LLM can use tools and apps, and asks before it changes anything.

### Added
- `/bin/agent` (`user/agent.c`), a model-independent agent pipeline:
  - Tier 0 rules handle common requests without a model ("sort my downloads", "stop spin", "what is wrong", "list /home", "find todo")
  - Otherwise the language model plans with Qwen's own tool-calling format (`<tools>` in the system prompt, `<tool_call>` answers, `<tool_response>` results) in a loop of up to 8 steps, then answers
  - While the model writes a tool or app name, only real names can be chosen
  - 12 tools: `list_folder`, `read_file`, `find_files`, `system_status`, `memory_search` (read, run at once) and `write_file`, `make_folder`, `move`, `copy`, `stop_program`, `lower_priority`, `run_app` (change, ask y/n first)
  - Writes only inside `/home` and `/tmp`; `knocsh`, `healthd` and `agent` can't be stopped; no delete and no raw shell
  - Every change is logged in the memory graph (`agent --action--> ...`, `stopped`, `lowered`)
  - `agent --tools`, `agent --llm TASK` (skip the rules), `agent --call JSON` (run one tool call without a model)
- App manifests: `/etc/apps/<name>.app` (from `apps/` in the source tree) describe an app (name, description, usage, risk). Every app with a manifest becomes usable by the agent through `run_app`; `organize`, `hello` and `modelcheck` have one
- Installed apps: any program in `/bin` can run, even if the kernel doesn't know it, as a NORMAL program with console, file and memory rights only
- Output capture: `spawn_capture` and `captured` system calls; the app's output still goes to the screen and a copy (up to 4 KiB) goes to the program that started it
- `user/llm.c`: the LLM engine shared by `ask` and `agent`; the model is chosen by `/etc/llm.model` (default `/models/qwen.kllm`), the context is 2048 tokens, special tokens like `<tool_call>` are encoded as single tokens like Qwen's own tokenizer, and the prompt cache stores the model header so a different model never reuses it
- `make test` Run 6: rules, y/n, apps, blocked writes, direct tool calls and graph logging, without a model

### Changed
- Shell lines are up to 256 characters and 32 words; program arguments up to 256 characters

### Fixed
- CI: the shell tests typed commands on a timer and powered off at a fixed time, so on a slower machine (GitHub Actions) commands ran into each other and the output after `organize --undo` was lost. `scripts/drive.py` now types each command only when the shell prompt is back
- `healthd` blamed the shell for disk activity that was the kernel's own work (memory graph writes, loading programs); disk problems are now only blamed on a program that read or wrote files


## [0.17.0] - 2026-09-24

Self-healing, and an LLM that knows this computer.

### Added
- `healthd` fixes what it finds (recover mode, the default): a CPU hog or disk thrashing program is moved to background priority; a memory leak is stopped after 10 seconds of steady growth; a spawn storm is stopped; a program filling the disk is stopped when the disk is 90% full or would be full within 60 seconds. `knocsh` and `healthd` are never touched. Every fix goes into the memory graph (`stopped`, and the new `lowered` link)
- `health watch` (only report) and `health recover` (report and fix), stored in `/etc/health.mode`; `health` shows the mode and the recent problems and fixes
- `setclass` system call (`SYSTEM` capability): lowers the priority class of a user program; it can never raise one
- `ps` marks the foreground program (the one the shell is waiting for)
- Per-program disk counter: telemetry names the program that read and wrote the most file data (`top_disk`), shown by `health` as "most disk"; disk problems blame it
- GraphRAG for `ask` (`user/rag.c`): the question decides what is needed. Names in the question (files, programs, drivers, apps) are looked up in the memory graph and their links become facts; health words bring the current telemetry and recent problems and fixes; crash words bring the AI space status and the black box reports; file words bring recent moves. Facts are plain sentences (a move and the file's type become "moved X from A to B because it is a document file"), shown to the user and given to Qwen before the question. General questions get no facts
- `ask` saves the attention cache of the fixed system prompt in `/tmp/ask-prefix.kv` and reuses it, so each question skips about 40 prompt tokens
- The health model is multi-label: 5 outputs, one per problem (sigmoid), so two problems at the same time are both found. The collector covers every pair of problems
- `make test`: `ask` retrieval on Boot 1; Run 5 checks recovery (leak stopped, CPU hog lowered, spawn storm stopped at the same time as the hog)

### Changed
- Health features use the change of RAM and disk use inside the 10 second window instead of how full they are, so a big or full disk is not a problem by itself
- The blamed program is the one that was on top for that problem in most of the 10 seconds, not only the last second, and it must still be running
- No alert without a program to blame, the foreground program is never a CPU hog (the user asked for that work), and the same problem in the same program waits 60 seconds before it can be reported again
- "Disk filling up" needs free space that really goes down (at least 512 KiB in 10 seconds, falling in 5 of them); a program that is already in the background is not lowered again
- Health facts for `ask` join each problem with its fix ("The program spin was hogging the CPU; healthd fixed it by moving it to background priority."), which a small model reads more reliably
- Scheduler: a woken process runs right away when its class is higher than the running one (wake-up preemption). With a background CPU hog running, loading the LLM took 54 s; now 6 s
- `memory find` ignores upper and lower case
- `knm.py`: `raw_logits`; `nn.c`: `nn_logits`

## [0.16.0] - 2026-09-24

The anomaly detector, and the first LLM inside KnocOS.

### Added
- Kernel telemetry (`kernel/telemetry.c`): a background process takes one sample per second (CPU busy, process switches, system calls, denied calls, processes, programs started, crashes, disk reads, writes and wait, free RAM, user memory, free disk, and the top program for CPU, memory, system calls and starts), keeping the last 600; `telemetry` system call with the `SYSTEM` capability
- The anomaly detector: `models/health/` (labeled data collection in QEMU, 48 window features over 10 seconds, a 48→64→32→6 int8 MLP, leave-one-run-out tests) and `/bin/healthd`, a background daemon that finds memory leaks, CPU hogs, disk thrashing, spawn storms and a filling disk, names the program, says when a problem is over and writes it to the memory graph (`anomaly` links)
- A memory leak must grow in at least 6 of the last 10 seconds, so loading a big model is not a leak
- Load programs to train and test it: `leak`, `spin`, `diskload`, `quiet`, `spawner`, `filler`, `recorder`
- Shell: `health`, `sleep N`, `kill NAME`, `ask QUESTION`
- The LLM: `models/llm/export.py` turns Qwen2.5-0.5B-Instruct into `qwen.kllm` (int8, groups of 64, with its byte-level BPE tokenizer, 528 MB); `models/llm/reference.py` is the numpy reference; `/bin/ask` is our own C inference engine (BPE tokenizer, int8 matmul, RMSNorm, RoPE, grouped-query attention with a KV cache, SwiGLU, ChatML prompt, streaming answers)
- `make reset-disk DISK_MB=1024` (or larger) puts the LLM on the disk
- `make test`: Run 5 checks the anomaly detector on a memory leak and a CPU hog, and Boot 1 checks there are no false alarms

### Changed
- Sleep locks are fair: a released lock goes to the process that has waited longest, so the telemetry thread is never starved by a disk-heavy program
- Free disk space is a counter, read without the filesystem lock
- Faster disk: requests of up to 128 KiB, file reads in runs of contiguous blocks and pages
- `knm.save` takes a `config`; the weight scale grows when a bias would not fit in int32

## [0.15.0] - 2026-09-24

The memory graph: one shared memory for every AI model.

### Added
- KnocGraph (`kernel/memgraph.c`): a knowledge graph in the kernel, stored in `/memory` on KnocFS (header, 4,096 nodes of 128 bytes, 16,384 links of 32 bytes as a ring), loaded into RAM with a hash index. Nodes are unique by kind and name (file, folder, program, driver, crash, type, source, actor, diagnosis, action, capability); links carry the relation, the actor that recorded them, a confidence, the boot number and the time. Repeating a fact refreshes it
- `graph` system call (record, stats, recent, links of a node, find, forget) with a `KNOWLEDGE` capability; the kernel fills in the actor from the calling program; forget also needs `SYSTEM`
- Producers: `organize` (classified_as, came_from, moved_to, restored_to), the kernel (started programs), security checks (denied capabilities), the guardian (crashed, crashed_in, restarted, stopped, disabled) and black box crash reports imported at boot (in_process, diagnosed_as, action)
- Shell: `memory`, `memory recent [N]`, `memory find TEXT`, `memory show NAME`, `memory why FILE`, `memory forget NAME`
- The disk boot counter is the graph's boot number
- `make test`: `memory why` after organize, the graph after a reboot, crash reports and the disabled driver after Run 3
- Roadmap to the GUI in `goal.md`

### Fixed
- The test script's `check` passed patterns starting with `--` to grep as options

## [0.14.0] - 2026-09-24

The first AI model inside KnocOS: the file organizer.

### Added
- `models/`: dataset script (`data/make_filedata.py`) and the file classifier (`filenet/`): features, numpy training with a settings search and confidence calibration, the int8 `.knm` model format, `organize.py` and `realcheck.py` for testing on real files
- `/bin/organize`: classifies every file in a folder (type and source), then the AI (at least 90% sure), a rules layer (extensions, signatures, app name patterns, readable text) or `Random/` picks the folder; plan by default, `--apply` moves, `--undo` restores
- The trained model on the disk at `/models/filenet.knm`, and sample files in `/home/Downloads`
- Floating-point support for programs: `mstatus.FS` is enabled and the 32 FP registers and `fcsr` are saved and restored on every process switch
- Program arguments: `spawn(name, args)` and `getargs`; the shell passes everything after the program name (paths resolved against the current directory)
- `rename(from, to)` system call and `knocfs_rename`: moves files and folders by changing directory entries
- `make test`: organize plan, apply, check and undo on the sample Downloads

### Removed
- Unused crash data collection code (crash labels, `random` / `inject` system calls, `DEBUG` capability, collect mode mailbox fields)

## [0.13.0] - 2026-09-23

The shell: KnocOS can be used by typing commands.

### Added
- `knocsh` (`user/knocsh.c`), the shell: `ls`, `cd`, `pwd`, `cat`, `echo [> file]`, `mkdir`, `rm`, `ps`, `kill`, `run NAME [&]` or just the program name, `mem`, `devices`, `crashes`, `ai`, `uptime`, `clear`, `help`, `exit`; relative paths with `.` and `..`; line editing with Backspace
- Terminal layer (`kernel/tty.c`): the console process keeps the control keys and passes the others to the program that owns the terminal (`PROGRAM_TERMINAL`); `read(0)` reads from it
- Ctrl-C kills the foreground program (the one the shell is waiting for)
- System calls `wait`, `ps`, `kill`, `sysinfo`, `devinfo`, `crashinfo` and the `SYSTEM` capability (all but `wait` need it); errors `E_CRASHED`, `E_KILLED`
- Guardian helpers for the shell: AI space status (uptime, warm restarts, disabled drivers, safe mode) and black box crash reports
- `counter` program (prints forever, for Ctrl-C)
- `make test`: a typed shell session on Boot 1, a file written by the shell read back on Boot 2, and `crashes` + `ai` in safe mode after Run 3

### Changed
- The console starts the shell (also in safe mode) instead of only echoing keys; it echoes itself only while no shell runs, and doesn't start a second shell when it is restarted after a crash
- `process_kill` releases the killed process's sleep locks, sets `E_KILLED` as its exit code and wakes waiters; the shell can only kill user programs
- `process_wait` tells a crashed program apart from a missing one
- The test harness waits for the shell and types whole command lines

## [0.12.0] - 2026-09-23

Wait queues and the KnocFS filesystem: programs and AI models live on disk.

### Added
- Wait queues: `process_block(channel, timeout)` / `process_wake(channel)`, a `BLOCKED` state, and `scheduler_preempt()` after device interrupts so an interactive process runs as soon as it's woken
- Sleep locks (`sleeplock_acquire` / `release`); a process that crashes has its sleep locks released
- The console and the `read` system call sleep until a key arrives (the UART interrupt wakes them); `process_wait()` sleeps until the process exits; the guardian is woken at once by a crash
- Disk: requests of up to 8 sectors (`read_blocks` / `write_blocks`, `device_read_blocks` / `device_write_blocks`), a sleep lock, and the requesting process sleeps until the completion interrupt
- KnocFS (`kernel/knocfs.c`): superblock, free-block bitmap, 1024 inodes with up to 12 extents each, directories, absolute paths; `mount`, `lookup`, `create`, `remove`, `stat`, `readdir`, `truncate`, `read`, `write`, `usage`
- File system calls: `open`, `close`, `seek`, `stat`, `readdir`, `mkdir`, `remove`; `read`/`write` take a file descriptor (0 keyboard, 1–2 screen, 3+ files); capabilities `FILES_READ` / `FILES_WRITE`; 8 open files per process; file I/O goes directly into the program's pages
- Programs load from `/bin/<name>` on disk, with the built-in copies as a fallback
- Host tool `tools/knocfs.py` (format, mkdir, put, put-text, ls, cat, rm, info, make-test-model) and `scripts/mkdisk.sh`; `make put`, `make ls`, `make sync-programs` (run by `make run`)
- Programs `files` (a note that survives reboots, `/bin` listing, 20 KB write/read/remove) and `modelcheck` (AI_AGENT, loads an 8 MiB model file and checks every byte)
- Self-tests: 200 disk reads while another process runs, a multi-sector read equal to 8 single reads, programs loaded from disk
- `make test`: the Boot 1 note is found on Boot 2

### Changed
- `disk.img` is 64 MiB (`DISK_MB`) with a KnocFS filesystem; `make reset-disk` builds it with `scripts/mkdisk.sh`
- `process_restart()` reloads a program outside the interrupts-off region (it may read from disk)
- `noperm` also checks that `open` is refused without a FILES capability

### Fixed
- Program data segments could start inside the last code page; the loader then mapped a zeroed page over the code. User programs now page-align their data segment, and `elf_load()` rejects segments that share a page
- Two processes using the disk at the same time could mix up their requests (shared descriptors and buffer); the disk now has a sleep lock
- `make reset-disk` with a bad size (for example `DISK_MB=4096.`) deleted `disk.img` before failing; `mkdisk.sh` now checks the size first

## [0.11.0] - 2026-09-23

User mode and system calls: programs can't touch the kernel.

### Added
- User programs run in U-mode with their own page table (`vm_user_create`): the kernel's root entries are shared without the U bit, and the user slots (`0x1000000000`–`0x1FFFFFFFFF`) are private. The scheduler switches `satp` on every process switch
- Trap entry from U-mode: `sscratch` holds the process's kernel stack while a program runs (0 in the kernel); `supervisor_trap` swaps it with `sp` and returns with `sret` to the right mode. `user_enter()` starts a program with all registers cleared
- 9 system calls (`kernel/syscall_abi.h`): `exit`, `write`, `read`, `getpid`, `yield`, `sleep`, `uptime`, `spawn`, `mem_alloc`, with error codes
- Safe user copies: every user pointer is translated in the program's own page table (user page + right permission required), so bad pointers return `E_FAULT`
- Capabilities per program (`CONSOLE`, `SPAWN`, `MEMORY`): denied calls are refused and logged `[SECURITY]`
- Memory quotas: 16 MiB for normal programs, 1 GiB for `AI_AGENT` programs; blocks are zeroed and mapped with megapages when large
- System call trace: the last 8 calls and the number of denied calls per process go to the AI space with a crash report. The AI prints them, diagnoses user faults (null pointer, own memory, outside its space) and does not restart a program that made forbidden calls (the kernel's fallback rule does the same)
- ELF loader (`kernel/elf.c`) and built-in programs embedded with `.incbin` (`kernel/programs.S`, `kernel/program.c`)
- User library (`user/crt0.S`, `user/ulib.c`) and 7 programs: `hello`, `badcall`, `noperm`, `hog`, `bigmem`, `crash`, `spy`
- Self-test: 5 programs must exit with 0, and every page must be returned afterwards (no leaks)
- Test keys: Ctrl-U (`crash` program), Ctrl-E (`spy` program)
- `process_spawn`, `process_wait`, `process_exit_code`; a `LOADING` state so a program isn't scheduled before it's loaded
- `make test`: user program checks, and a failure if `spy` ever reads kernel memory

### Changed
- `memset` writes 8 bytes at a time (large program blocks are cleared quickly)
- `vm.c` mapping functions take a root table, so the same code maps the kernel and user programs
- `boot.S` clears `sscratch` at boot

## [0.10.0] - 2026-09-23

Big memory: room for AI models.

### Added
- Device tree parser (`kernel/fdt.c`): RAM start/size from `/memory` and the CPU count from `/cpus`. QEMU passes the device tree address in `a1`; `boot.S` hands it to `kernel_main(dtb)` and `aispace_main(dtb)`
- Buddy page allocator (`kernel/page.c`): blocks of 4 KiB up to 1 GiB (orders 0–18), splitting on allocation and merging buddies on free, one metadata byte per page. New `page_alloc_order()`, `page_alloc_contiguous()`, `page_largest_free()`, `page_ram_start()` / `page_ram_end()`
- 2 MiB megapages: `vm_map_range()` maps with level-1 leaf entries when aligned (2 GiB of RAM = 1026 entries). `vm_map()` refuses to map inside a megapage, and `vm_debug()` understands them
- Spinlocks (`kernel/spinlock.c`): `spin_lock` / `spin_unlock` (interrupts off while held), `spin_trylock`, used by the heap and the page allocator
- Shared UART lock between the kernel and the AI space: a core owns the UART for a whole line (`console_owner` in the mailbox, compare-and-swap, 10 ms timeout)
- Self-tests: spinlock, a 64 MiB aligned contiguous block, 1024 single pages freed and merged back, the largest free block (1 GiB) written and read through megapages
- `make test` checks the RAM size, CPU count, megapage count and that kernel and AI space output never mix on one line

### Changed
- QEMU runs with 2 GiB (`-m 2G`); KnocOS needs at least 1 GiB and works with 8 GiB+
- AI space: 256 MiB at `0x90000000` (was 16 MiB at `0x87000000`)
- Kernel heap virtual region moved to `0x2000000000` (`0x90000000` is now real RAM)
- On a warm restart the AI space restores the device tree address in `a1` before jumping to `_start`
- The AI's "bad pointer" diagnosis uses the real RAM end (published by the kernel in the mailbox)
- `memory.c` helpers and `make pages` / `make size` use the real memory layout instead of 128 MiB

## [0.9.0] - 2026-09-23

Fault containment and warm kernel restart: the AI never stops.

### Added
- Fault containment: an exception in a process that ran with interrupts on stops only that process (`[OOPS]`, state `CRASHED`), like a Linux oops. Kernel code, interrupt handlers and code with interrupts off still count as a kernel crash
- AI verdicts for process crashes: the kernel's `guardian` process posts each crashed process to the mailbox (`fault_seq`), the AI space diagnoses it and replies (`verdict_seq`) with `RESTART_PROCESS` and/or `DISABLE_DRIVER`. After 3 restarts a process is left stopped. Without the AI space the kernel applies the same rules after a 1 s timeout
- `process_crash`, `process_next_crash`, `process_restart` (in place, same stack, new pid) and `process_discard`
- Driver tracking: every call into a driver (init, interrupt, read, write, block I/O) records the active driver per process, so crash reports say which driver crashed
- `device_disable()` and `plic_disable()`. Drivers on the AI space's disabled list are skipped at boot (`Device disabled by the AI space`)
- `faulty0`: a test driver with a bug on purpose (`kernel/faulty.c`)
- Warm kernel restart: the AI space copies the clean kernel image (`kernel_start`..`kernel_image_end`, 38 KiB) into its protected memory at boot. On a crash or freeze it stops core 0 with a machine software interrupt, parks it in AI space code (`aispace_park_core0`), saves the black box, restores the clean kernel and restarts only core 0 at `_start`
- Boot handshake (`boot_request` / `boot_ack`): core 0 waits in `boot.S` until the AI space has taken its clean copy (1 s timeout when there is no AI space)
- Kernel code check: before restoring, the AI space compares the running kernel code with the clean copy and reports corrupted bytes in its diagnosis
- New black box actions: warm restart, warm restart into safe mode. Records now include the driver name
- Halting after 4 crashes in a row keeps core 0 stopped while the AI space stays online
- Test keys: Ctrl-X (driver fault), Ctrl-K (kernel fault with interrupts off), Ctrl-O (overwrite kernel code)
- `make test` run 3: containment, driver disabling, freeze / code corruption / kernel fault → warm restarts #1–#3 → safe mode, with no machine reboot. Run 4: the 4th crash halts the kernel and the AI stays online

### Changed
- Ctrl-F now crashes only the console process (contained) instead of the kernel
- Crash recovery uses a warm kernel restart; a full machine reboot is only the fallback when core 0 can't be stopped
- `panic()` turns interrupts off first, so no other process runs after a panic
- The `guardian` process runs for the whole uptime (fault handling + the 60 s crash-streak reset)
- `plic_init()` clears all IRQ enables, and `plic_enable()` completes any interrupt a crashed kernel left claimed
- `boot.S` clears `sstatus.SIE` at boot and enables `mie.MSIE` on core 0
- Diagnosis texts are neutral ("the code accessed ...") since they now describe processes too

## [0.8.0] - 2026-09-23

The AI space: AI that survives kernel crashes.

### Added
- AI space on CPU core 1 (`kernel/aispace.c`): runs in M-mode in its own 16 MiB region at `0x87000000`, self-contained (own UART output, clock, polled virtio-blk driver), linked into its own sections
- PMP protection: the kernel (core 0, S-mode) cannot read, write or execute the AI space. A boot self-test probes it
- Kernel ↔ AI space mailbox (`kernel/mailbox.h`): heartbeat, uptime, current process, crash details, and `last_kernel_pc` written by the M-mode timer
- Crash detection (panic and unhandled traps) and freeze detection (no heartbeat for 2 s)
- Rule-based diagnosis (Tier 0 brain): null pointer, bad pointer, page fault, protected memory, illegal instruction, misaligned access, panic, infinite loop
- Black box (`kernel/blackbox.h`): crash reports in the last 8 sectors of the disk, written by the AI space; the kernel prints new reports at boot (`kernel/guardian.c`)
- Recovery actions: reboot, safe mode after 3 crashes in a row, halt after 4 (boot-loop protection); a `guardian` process resets the streak after 60 s without a crash
- Test keys: Ctrl-F (bad pointer), Ctrl-P (panic), Ctrl-W (freeze)
- `trap_probe_read()`: a read that returns an error instead of crashing on a bad address
- `make test` boots 3–6: fault → freeze → panic → safe mode, all detected, diagnosed, saved and recovered
- Milestone roadmap v0.8 → v1.0 in `goal.md` and README

### Changed
- QEMU runs with 2 cores (`-smp 2`)
- `boot.S`: routes core 1 to the AI space, clears `.bss`, sets 2 PMP entries, resets `satp` and interrupt state on every boot
- Linker script: AI space sections, explicit small-data sections, `bss_start`/`bss_end`, a `.mailbox` section
- The page allocator stops below the AI space
- `panic()` and unhandled traps report to the AI space

### Fixed
- `.bss` was never cleared: it only worked because the first boot started from zeroed RAM. A warm reboot kept old values
- After a warm reboot, paging (`satp`) was still on from the previous boot, so the new kernel corrupted its own live page tables

## [0.7.0] - 2026-09-23

Processes and the AI-aware scheduler.

### Added
- Processes (kernel threads) with their own 16 KiB stacks, saved context, class, state and CPU accounting (`kernel/process.c/h`)
- `context_switch` in assembly (`kernel/switch.S`)
- Timer-driven preemption: `scheduler_tick()` on every tick
- AI-aware scheduler: INTERACTIVE first, then weighted virtual-runtime sharing between AI_AGENT (60), NORMAL (30) and BACKGROUND (10), with per-class time slices and no starvation
- `process_create`, `process_yield`, `process_sleep`, `process_exit`, `process_kill`, `process_list`
- Scheduler self-test: three CPU-bound workers must get 60/30/10 (±10) of the CPU, all must progress, and an interactive process must wake on its target tick
- The keyboard echo runs as an INTERACTIVE `console` process
- `memcpy` / `memset` (`kernel/string.c`), required by the compiler for struct copies

### Changed
- M-mode uses its own private stack through `mscratch`, so the timer handler never touches process stacks
- The S-mode trap handler saves and restores `sepc` and `sstatus`, so switching processes inside a trap is safe
- `kernel_main` becomes the idle process (pid 0) after boot

## [0.6.0] - 2026-09-23

Disk driver: KnocOS has permanent storage.

### Added
- virtio-blk disk driver (`kernel/virtio_blk.c`), registered as block device `disk0` (IRQ 1): feature negotiation, an 8-entry virtqueue, interrupt-driven 512-byte sector reads and writes
- virtio-mmio definitions (`kernel/virtio.h`)
- Block devices in the device model: `block_size`, `block_count`, `read_block`, `write_block`, and `device_read_block` / `device_write_block` with range checks
- `disk.img` (1 MiB) created by the Makefile and attached by `make run`, plus `make reset-disk`
- Disk self-test: write/read-back of a sector, reading text placed by the host, and a boot counter stored on the disk
- `make test` boots twice on a fresh test disk and checks the boot counter goes from 1 to 2 (data survives reboots)
- `log_info_uint` and `log_info_text` logging helpers

### Changed
- The virtio slot (`0x10001000`) is mapped in the kernel page tables
- `device_list` shows the block count of block devices
- No changes were needed in `trap.c` or `plic.c` to add the disk

## [0.5.0] - 2026-09-23

Device abstraction. Phase 4 (hardware and interrupts) is complete.

### Added
- Device abstraction (`kernel/device.c/h`): a common `device_t` driver shape (`init`, `interrupt`, `read`, `write`) and a device table
- `device_register`, `device_init_all`, `device_find`, `device_read`, `device_write`, `device_handle_irq`, `device_list`
- UART registered as device `uart0` (IRQ 10), power device as `power0`
- Boot log lists every device, and a self-test checks the device table
- `make test` checks that `uart0` and `power0` are ready

### Changed
- The trap handler dispatches device interrupts through the device table instead of hard-coding the UART IRQ
- `kernel_main` starts devices with `device_init_all()`, and the echo loop uses `device_read` / `device_write`
- UART internals (`init`, `interrupt`, `getc`) are private to the driver, and only the early-console output functions stay public

## [0.4.0] - 2026-09-23

Hardware interrupts and traps.

### Added
- Supervisor-mode trap handler in C with exception decoding (`scause`, `sepc`, `stval`)
- Exceptions delegated to S-mode (`medeleg`)
- Timer interrupts forwarded from M-mode to the kernel (`mip.SSIP` + `mideleg`)
- PLIC interrupt controller driver (`plic_enable`, `plic_claim`, `plic_complete`)
- Interrupt-driven UART input with a 128-byte ring buffer and keyboard echo
- Power-off / reboot driver (QEMU test device); Ctrl-D powers off
- `make test`: automated boot test that checks every self-test and powers off
- GitHub Actions CI: build + `make test` on every push
- Version number shown at boot (`VERSION` file)

### Changed
- Timer read with `rdtime`, first deadline armed in M-mode, drift-free re-arming
- Timer constants shared between C and assembly (`timer.h`)
- One UART driver (`uart.c`) replaces three copies of UART code
- Makefile: automatic header dependencies (`-MMD -MP`), `-Wall -Wextra -Werror`
- Linker script: separate `R-X` and `RW-` segments

### Fixed
- Timer self-test failing because it depended on CPU speed

## [0.3.0] - 2026-09-13

Kernel heap.

### Added
- `kmalloc` / `kfree` in a dedicated virtual region (`0x90000000`)
- Block splitting, coalescing and automatic page growth
- Heap stress test

## [0.2.0] - 2026-09-13

Physical and virtual memory.

### Added
- Bitmap physical page allocator (`page_alloc` / `page_free`)
- Sv39 page tables, identity-mapped kernel, paging enabled

## [0.1.0] - 2026-09-10

First boot.

### Added
- RISC-V boot code, linker script, stack, C kernel entry
- UART output and basic memory accounting
- Make-based build and QEMU `virt` support
