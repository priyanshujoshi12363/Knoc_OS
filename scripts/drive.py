#!/usr/bin/env python3
import os
import re
import select
import subprocess
import sys
import time

PROMPT = re.compile(rb"knoc:\S*\$ ")
BOOT_WAIT = 60
COMMAND_WAIT = 60
RUNNING_WAIT = 2


def main():
    timeout, log_path, disk, kernel = float(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
    commands = sys.argv[5:]
    qemu = subprocess.Popen(
        ["env", "-i", "PATH=/usr/bin:/bin:/usr/sbin:/sbin", "qemu-system-riscv64",
         "-machine", "virt", "-smp", "2", "-m", "2G", "-bios", "none", "-nographic",
         "-global", "virtio-mmio.force-legacy=false",
         "-drive", f"file={disk},if=none,format=raw,id=disk0",
         "-device", "virtio-blk-device,drive=disk0,bus=virtio-mmio-bus.0",
         "-kernel", kernel],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = bytearray()
    deadline = time.time() + timeout

    def pump(until):
        with open(log_path, "ab") as log:
            while time.time() < min(until, deadline):
                ready, _, _ = select.select([qemu.stdout], [], [], 0.1)
                if ready:
                    data = os.read(qemu.stdout.fileno(), 65536)
                    if not data:
                        return False
                    output.extend(data)
                    log.write(data)
        return True

    def wait_for_prompts(count, seconds):
        until = time.time() + seconds
        while len(PROMPT.findall(output)) < count and time.time() < min(until, deadline):
            if not pump(time.time() + 0.2):
                return

    def send(data):
        try:
            qemu.stdin.write(data)
            qemu.stdin.flush()
        except BrokenPipeError:
            pass

    open(log_path, "wb").close()
    wait_for_prompts(1, BOOT_WAIT)

    for index, command in enumerate(commands):
        prompts = len(PROMPT.findall(output))
        if command == "^C":
            send(b"\x03")
            wait_for_prompts(prompts + 1, COMMAND_WAIT)
            continue
        send(command.encode() + b"\r")
        if index + 1 < len(commands) and commands[index + 1] == "^C":
            pump(time.time() + RUNNING_WAIT)
        else:
            wait_for_prompts(prompts + 1, COMMAND_WAIT)

    pump(time.time() + 1)
    send(b"\x04")
    pump(deadline)

    try:
        return qemu.wait(timeout=max(1, deadline - time.time()))
    except subprocess.TimeoutExpired:
        qemu.kill()
        qemu.wait()
        return 124


if __name__ == "__main__":
    sys.exit(main())
