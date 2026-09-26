import argparse
import csv
import os
import random
import re
import select
import subprocess
import sys
import tempfile
import time
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DATA = os.path.join(HERE, "data")

LABELS = ["null_pointer", "bad_pointer", "unallocated", "stack_overflow", "bad_jump",
          "illegal_instruction", "misaligned", "code_corruption", "kernel_panic", "kernel_freeze"]

USER_MODES = {"null": "null_pointer", "wild": "bad_pointer", "unmapped": "unallocated",
              "stack": "stack_overflow", "jump": "bad_jump", "illegal": "illegal_instruction",
              "misaligned": "misaligned"}

KERNEL_KEYS = [("\x06", "bad_pointer"), ("\x18", "bad_pointer"), ("\x17", "kernel_freeze"),
               ("\x0f", "code_corruption"), ("\x0b", "bad_pointer"), ("\x10", "kernel_panic")]

PROMPT = re.compile(rb"knoc:\S*\$ ")
DATA_LINE = re.compile(rb"\[CRASHDATA\] ([0-9,]+)")


class Machine:
    def __init__(self, seed):
        self.disk = tempfile.mktemp(suffix=".img")
        subprocess.run(["./scripts/mkdisk.sh", self.disk], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        command = ["qemu-system-riscv64", "-machine", "virt", "-smp", "2", "-m", "2G", "-bios", "none",
                   "-nographic", "-global", "virtio-mmio.force-legacy=false",
                   "-drive", f"file={self.disk},if=none,format=raw,id=disk0",
                   "-device", "virtio-blk-device,drive=disk0,bus=virtio-mmio-bus.0",
                   "-kernel", "knocos.elf", "-append", "crashdata"]
        self.qemu = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self.output = bytearray()
        self.log = open(os.path.join(DATA, f"raw-{seed}.log"), "wb")

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([self.qemu.stdout], [], [], 0.1)
            if ready:
                data = os.read(self.qemu.stdout.fileno(), 65536)
                if not data:
                    return
                self.output.extend(data)
                self.log.write(data)

    def wait_prompt(self, count, seconds=30):
        end = time.time() + seconds
        while len(PROMPT.findall(self.output)) < count and time.time() < end:
            self.pump(0.2)

    def send(self, text):
        self.qemu.stdin.write(text.encode())
        self.qemu.stdin.flush()

    def rows(self):
        return DATA_LINE.findall(self.output)

    def close(self):
        try:
            self.send("\x04")
        except BrokenPipeError:
            pass
        self.pump(2)
        self.qemu.kill()
        self.log.close()
        os.remove(self.disk)


def user_run(job):
    seed, rounds = job
    rng = random.Random(seed)
    machine = Machine(seed)
    machine.wait_prompt(1)
    samples = []
    for _ in range(rounds):
        modes = list(USER_MODES)
        rng.shuffle(modes)
        for mode in modes:
            before = len(machine.rows())
            prompts = len(PROMPT.findall(machine.output))
            machine.send(f"crash {mode} {rng.randint(1, 10 ** 6)}\r")
            machine.wait_prompt(prompts + 1)
            machine.send("sleep 1\r")
            machine.wait_prompt(prompts + 2)
            machine.pump(0.3)
            for row in machine.rows()[before:]:
                samples.append((seed, USER_MODES[mode], row.decode()))
    for _ in range(4):
        before = len(machine.rows())
        prompts = len(PROMPT.findall(machine.output))
        machine.send("crash\r")
        machine.wait_prompt(prompts + 1)
        machine.send("sleep 1\r")
        machine.wait_prompt(prompts + 2)
        machine.pump(0.3)
        for row in machine.rows()[before:]:
            samples.append((seed, "null_pointer", row.decode()))
    before = len(machine.rows())
    prompts = len(PROMPT.findall(machine.output))
    machine.send("spy\r")
    machine.wait_prompt(prompts + 1)
    machine.pump(2)
    for row in machine.rows()[before:]:
        samples.append((seed, "bad_pointer", row.decode()))
    machine.close()
    return samples


def kernel_run(job):
    seed, _ = job
    machine = Machine(seed)
    machine.wait_prompt(1)
    samples = []
    for key, label in KERNEL_KEYS:
        before = len(machine.rows())
        machine.send(key)
        end = time.time() + 12
        while len(machine.rows()) == before and time.time() < end:
            machine.pump(0.3)
        machine.pump(5)
        rows = machine.rows()[before:]
        if len(rows) != 1:
            print(f"run {seed}: {len(rows)} crash lines after key {key!r}, skipping the rest", file=sys.stderr)
            break
        samples.append((seed, label, rows[0].decode()))
    machine.close()
    return samples


def run(job):
    kind, seed, rounds = job
    return user_run((seed, rounds)) if kind == "user" else kernel_run((seed, rounds))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--user-runs", type=int, default=6)
    parser.add_argument("--rounds", type=int, default=8)
    parser.add_argument("--kernel-runs", type=int, default=24)
    parser.add_argument("--parallel", type=int, default=6)
    parser.add_argument("--seed", type=int, default=100)
    args = parser.parse_args()

    os.makedirs(DATA, exist_ok=True)
    jobs = [("user", args.seed + i, args.rounds) for i in range(args.user_runs)]
    jobs += [("kernel", args.seed + 1000 + i, 0) for i in range(args.kernel_runs)]
    started = time.time()
    with Pool(args.parallel) as pool:
        results = pool.map(run, jobs)

    rows = [sample for result in results for sample in result]
    with open(os.path.join(DATA, f"crashes-{args.seed}.csv"), "w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(["run", "label", "features"])
        writer.writerows(rows)

    print(f"{len(rows)} crashes in {time.time() - started:.0f} s")
    for label in LABELS:
        print(f"  {label:<20} {sum(1 for row in rows if row[1] == label)}")


if __name__ == "__main__":
    sys.exit(main())
