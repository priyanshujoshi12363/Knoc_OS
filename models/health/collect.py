import argparse
import csv
import os
import random
import re
import subprocess
import sys
import tempfile
import time
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DATA = os.path.join(HERE, "data")

FIELDS = ["seq", "cpu_busy", "switches", "syscalls", "denied", "processes", "spawns", "crashes",
          "disk_reads", "disk_writes", "disk_wait", "ram_free_kib", "ram_total_kib",
          "user_memory_kib", "disk_free_kib", "disk_total_kib", "top_cpu", "top_mem_kib",
          "top_sys", "top_spawn", "top_cpu_name", "top_mem_name", "top_sys_name", "top_spawn_name"]
NUMERIC = 20

LABELS = ["normal", "memory_leak", "cpu_hog", "disk_thrash", "spawn_storm", "disk_filling"]

NORMAL_COMMANDS = ["ls /bin", "cat /hello.txt", "memory", "ps", "hello", "organize", "mem",
                   "uptime", "modelcheck", "ls /home/Downloads", "devices", "bigmem", "files",
                   "memory recent 3", "ai", "crashes"]

TRANSITION = 3


def problem(label, rng):
    if label == "memory_leak":
        return f"leak {rng.choice([128, 256, 512, 1024, 2048])} &", ["kill leak"]
    if label == "cpu_hog":
        return "spin &", ["kill spin"]
    if label == "disk_thrash":
        return "diskload &", ["kill diskload"]
    if label == "spawn_storm":
        return "spawner &", ["kill spawner"]
    return f"filler {rng.randint(24, 40)} &", ["kill filler", "rm /tmp/filler.bin"]


def busy(rng, low, high):
    steps = []
    end = rng.uniform(low, high)
    elapsed = 0.0
    while elapsed < end:
        pause = rng.uniform(4, 9)
        steps.append((pause, rng.choice(NORMAL_COMMANDS[:10])))
        elapsed += pause
    return steps


def script(seed, cycles):
    rng = random.Random(seed)
    steps = [(8.0, "recorder &")]
    problems = LABELS[1:]
    phases = [[label] for label in problems]
    phases += [[a, b] for i, a in enumerate(problems) for b in problems[i + 1:]]
    for _ in range(cycles):
        rng.shuffle(phases)
        for phase in phases:
            steps.append((1.0, "echo MARK normal"))
            steps.extend(busy(rng, 20, 32))
            if len(phase) == 2 and rng.random() < 0.5:
                phase = phase[::-1]
            first_start, stop = problem(phase[0], rng)
            steps.append((1.0, f"echo MARK {phase[0]}"))
            steps.append((0.5, first_start))
            steps.extend(busy(rng, 20, 30))
            if len(phase) == 2:
                other_start, other_stop = problem(phase[1], rng)
                both = "+".join(sorted(phase, key=LABELS.index))
                steps.append((1.0, f"echo MARK {both}"))
                steps.append((0.5, other_start))
                steps.extend(busy(rng, 20, 28))
                stop = stop + other_stop
            for command in stop:
                steps.append((0.5, command))
    steps.append((1.0, "echo MARK normal"))
    return steps


def run(job):
    seed, cycles = job
    disk = tempfile.mktemp(suffix=".img")
    log_path = os.path.join(DATA, f"raw-{seed}.log")
    subprocess.run(["./scripts/mkdisk.sh", disk], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["python3", "tools/knocfs.py", "mkdir", disk, "/etc"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    subprocess.run(["python3", "tools/knocfs.py", "put-text", disk, "/etc/health.mode", "watch"], cwd=ROOT,
                   check=True, stdout=subprocess.DEVNULL)
    command = ["env", "-i", "PATH=/usr/bin:/bin:/usr/sbin:/sbin", "qemu-system-riscv64",
               "-machine", "virt", "-smp", "2", "-m", "2G", "-bios", "none", "-nographic",
               "-global", "virtio-mmio.force-legacy=false",
               "-drive", f"file={disk},if=none,format=raw,id=disk0",
               "-device", "virtio-blk-device,drive=disk0,bus=virtio-mmio-bus.0",
               "-kernel", "knocos.elf"]
    with open(log_path, "wb") as log:
        qemu = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE, stdout=log, stderr=log)
        for pause, line in script(seed, cycles):
            time.sleep(pause)
            qemu.stdin.write(line.encode() + b"\r")
            qemu.stdin.flush()
        time.sleep(3)
        qemu.stdin.write(b"\x04")
        qemu.stdin.flush()
        try:
            qemu.wait(timeout=20)
        except subprocess.TimeoutExpired:
            qemu.kill()
    os.remove(disk)
    return log_path


def parse(log_path, seed):
    rows = []
    label = "normal"
    since_change = 0
    sample = re.compile(r"\[T\] ((?:\d+,){" + str(NUMERIC) + r"}[^,\s]*,[^,\s]*,[^,\s]*,[^,\s]*)")
    for line in open(log_path, errors="replace"):
        mark = re.search(r"MARK ([\w+]+)", line)
        if mark and all(part in LABELS for part in mark.group(1).split("+")) and not line.startswith("knoc:"):
            if mark.group(1) != label:
                label = mark.group(1)
                since_change = 0
            continue
        found = sample.search(line)
        if not found:
            continue
        values = found.group(1).split(",")
        since_change += 1
        if since_change <= TRANSITION:
            continue
        rows.append([seed, label] + values)
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--seed", type=int, default=300)
    args = parser.parse_args()

    os.makedirs(DATA, exist_ok=True)
    jobs = [(args.seed + i, args.cycles) for i in range(args.runs)]
    started = time.time()
    with Pool(args.runs) as pool:
        logs = pool.map(run, jobs)

    rows = []
    for (seed, _), log_path in zip(jobs, logs):
        rows.extend(parse(log_path, seed))

    with open(os.path.join(DATA, f"samples-{args.seed}.csv"), "w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(["run", "label"] + FIELDS)
        writer.writerows(rows)

    counts = {}
    for row in rows:
        counts[row[1]] = counts.get(row[1], 0) + 1
    print(f"{len(rows)} samples in {time.time() - started:.0f} s")
    for label, count in sorted(counts.items()):
        print(f"  {label:<26} {count}")


if __name__ == "__main__":
    sys.exit(main())
