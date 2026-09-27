#!/usr/bin/env python3
import os
import re
import select
import subprocess
import sys
import time

PROMPT = re.compile(rb"knoc:\S*\$ ")
QUESTION = re.compile(rb"\(y/n\) |you: |calc> |web> ")
BOOT_WAIT = 60
COMMAND_WAIT = int(os.environ.get("COMMAND_WAIT", "60"))
RUNNING_WAIT = 2


def main():
    timeout, log_path, disk, kernel = float(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
    commands = sys.argv[5:]
    qemu = subprocess.Popen(
        ["env", "-i", "PATH=/usr/bin:/bin:/usr/sbin:/sbin", "qemu-system-riscv64",
         "-machine", "virt", "-smp", "8", "-m", "2G", "-bios", "none", "-nographic",
         "-global", "virtio-mmio.force-legacy=false",
         "-drive", f"file={disk},if=none,format=raw,id=disk0",
         "-device", "virtio-blk-device,drive=disk0,bus=virtio-mmio-bus.0",
         "-netdev", "user,id=net0" + os.environ.get("QEMU_NETDEV_EXTRA", ""),
         "-device", "virtio-net-device,netdev=net0,bus=virtio-mmio-bus.1",
         "-device", "virtio-rng-device,bus=virtio-mmio-bus.2",
         "-kernel", kernel] + (["-append", os.environ["QEMU_APPEND"]] if os.environ.get("QEMU_APPEND") else []) +
        (["-device", "virtio-gpu-device,xres=1280,yres=800,bus=virtio-mmio-bus.3",
          "-device", "virtio-keyboard-device,bus=virtio-mmio-bus.4",
          "-device", "virtio-tablet-device,bus=virtio-mmio-bus.5",
          "-qmp", f"unix:{os.environ['QMP_SOCKET']},server=on,wait=off"] if os.environ.get("QMP_SOCKET") else []),
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

    def wait_for_text(pattern, count, seconds):
        until = time.time() + seconds
        while len(pattern.findall(output)) < count and time.time() < min(until, deadline):
            if not pump(time.time() + 0.2):
                return

    def qmp(messages):
        import json
        import socket
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(10)
        try:
            client.connect(os.environ["QMP_SOCKET"])
            reader = client.makefile("rb")
            reader.readline()
            for message in [{"execute": "qmp_capabilities"}] + messages:
                client.sendall(json.dumps(message).encode() + b"\n")
                while True:
                    reply = json.loads(reader.readline())
                    if "return" in reply or "error" in reply:
                        break
        except (OSError, ValueError):
            pass
        finally:
            client.close()

    names = {" ": "spc", "\n": "ret", "-": "minus", "=": "equal", ".": "dot", ",": "comma", "/": "slash",
             ";": "semicolon", "'": "apostrophe", "[": "bracket_left", "]": "bracket_right", "\\": "backslash",
             "`": "grave_accent", "\t": "tab"}
    shifted = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
               "_": "minus", "+": "equal", ":": "semicolon", '"': "apostrophe", "<": "comma", ">": "dot", "?": "slash",
               "{": "bracket_left", "}": "bracket_right", "|": "backslash", "~": "grave_accent"}

    def key_messages(text):
        messages = []
        index = 0
        while index < len(text):
            if text[index] == "{":
                end = text.index("}", index)
                combo = text[index + 1:end].split("+")
                messages.append({"execute": "send-key", "arguments": {"keys": [
                    {"type": "qcode", "data": part} for part in combo]}})
                index = end + 1
                continue
            c = text[index]
            index += 1
            keys = []
            if c.isupper():
                keys = ["shift", c.lower()]
            elif c in shifted:
                keys = ["shift", names.get(shifted[c], shifted[c])]
            else:
                keys = [names.get(c, c)]
            messages.append({"execute": "send-key", "arguments": {"keys": [
                {"type": "qcode", "data": k} for k in keys]}})
        return messages

    def pointer(x, y, button=None):
        events = [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 1280)}},
                  {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 800)}}]
        messages = [{"execute": "input-send-event", "arguments": {"events": events}}]
        if button:
            for down in (True, False):
                messages.append({"execute": "input-send-event", "arguments": {"events": [
                    {"type": "btn", "data": {"down": down, "button": button}}]}})
        return messages

    def screenshot(path):
        import json
        import socket
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(10)
        try:
            client.connect(os.environ["QMP_SOCKET"])
            reader = client.makefile("rb")
            reader.readline()
            for message in ({"execute": "qmp_capabilities"},
                            {"execute": "screendump", "arguments": {"filename": path}}):
                client.sendall(json.dumps(message).encode() + b"\n")
                while True:
                    reply = json.loads(reader.readline())
                    if "return" in reply or "error" in reply:
                        break
        except (OSError, ValueError):
            pass
        finally:
            client.close()

    questions_seen = [0]
    open(log_path, "wb").close()

    if os.environ.get("EARLY_SHOT"):
        import threading
        seconds, _, early_path = os.environ["EARLY_SHOT"].partition(":")
        threading.Timer(float(seconds), screenshot, [early_path]).start()
    wait_for_prompts(1, BOOT_WAIT)

    for index, command in enumerate(commands):
        prompts = len(PROMPT.findall(output))
        following = commands[index + 1] if index + 1 < len(commands) else ""
        if command == "^C":
            send(b"\x03")
            wait_for_prompts(prompts + 1, COMMAND_WAIT)
            continue
        if command.startswith("@shot "):
            pump(time.time() + 1.5)
            screenshot(command[6:])
            continue
        if command.startswith("@keys "):
            for message in key_messages(command[6:]):
                qmp([message])
                pump(time.time() + 0.05)
            pump(time.time() + 1)
            continue
        if command.startswith("@move ") or command.startswith("@click ") or command.startswith("@rclick "):
            kind, x, y = command.split()
            button = {"@click": "left", "@rclick": "right"}.get(kind)
            qmp(pointer(float(x), float(y), button))
            pump(time.time() + 1)
            continue
        if command.startswith("@drag "):
            _, x0, y0, x1, y1 = command.split()
            qmp(pointer(float(x0), float(y0)))
            qmp([{"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"down": True, "button": "left"}}]}}])
            pump(time.time() + 0.5)
            for step in range(1, 9):
                qmp(pointer(float(x0) + (float(x1) - float(x0)) * step / 8, float(y0) + (float(y1) - float(y0)) * step / 8))
                pump(time.time() + 0.3)
            qmp([{"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"down": False, "button": "left"}}]}}])
            pump(time.time() + 1)
            continue
        if command.startswith("@wait "):
            pump(time.time() + float(command[6:]))
            continue
        if command.startswith("?"):
            wait_for_text(QUESTION, questions_seen[0] + 1, COMMAND_WAIT)
            questions_seen[0] += 1
            send(command[1:].encode() + b"\r")
            if not following.startswith("?"):
                wait_for_prompts(prompts + 1, COMMAND_WAIT)
            continue
        questions_seen[0] = len(QUESTION.findall(output))
        send(command.encode() + b"\r")
        if following == "^C" or following.startswith("@"):
            pump(time.time() + RUNNING_WAIT)
        elif not following.startswith("?"):
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
