#!/usr/bin/env python3
"""KnocFS host tool: format a disk image and copy files onto it from Linux.

The layout must match kernel/knocfs.h:
  sectors [0..2047] boot test data | KnocFS | last 8 sectors: AI black box
  KnocFS blocks (4 KiB): superblock, free-block bitmap, inode table, data

Usage:
  knocfs.py format IMAGE
  knocfs.py mkdir IMAGE PATH...
  knocfs.py put IMAGE SOURCE DEST          copy a host file (replaces DEST)
  knocfs.py put-many IMAGE DEST_DIR FILE... copy several host files into one folder
  knocfs.py put-text IMAGE DEST TEXT
  knocfs.py make-test-model IMAGE DEST MIB  a file with a known byte pattern
  knocfs.py ls IMAGE [PATH]
  knocfs.py cat IMAGE PATH
  knocfs.py touch IMAGE PATH EPOCH          set a file's modified time
  knocfs.py put-link IMAGE TARGET DEST      make a symbolic link
  knocfs.py put-tree IMAGE HOSTDIR [DEST]   copy a folder with its links
  knocfs.py rm IMAGE PATH
  knocfs.py info IMAGE
"""

import os
import struct
import sys
import time

MAGIC = 0x31305346434F4E4B
VERSION = 1
BLOCK = 4096
SECTOR = 512
START_SECTOR = 2048
BLACKBOX_SECTORS = 8
EXTENTS = 12
NAME_MAX = 60
ROOT = 1
INODE_COUNT = 1024

TYPE_FREE, TYPE_FILE, TYPE_DIR, TYPE_LINK = 0, 1, 2, 3

SUPER = struct.Struct("<QIIQIIIIIIII")
INODE = struct.Struct("<HHIQ" + "II" * EXTENTS + "QQ")
DIRENT = struct.Struct("<I60s")

assert SUPER.size == 56 and INODE.size == 128 and DIRENT.size == 64


def test_model_byte(i):
    return (i * 7 + (i >> 12)) & 0xFF


class KnocFS:
    def __init__(self, path):
        self.file = open(path, "r+b")
        self.file.seek(START_SECTOR * SECTOR)
        fields = SUPER.unpack(self.file.read(SUPER.size))
        (magic, version, block_size, start, self.total_blocks, self.inode_count,
         self.bitmap_start, self.bitmap_blocks, self.inode_start, self.inode_blocks,
         self.data_start, _) = fields
        if magic != MAGIC or version != VERSION or block_size != BLOCK or start != START_SECTOR:
            sys.exit(f"{path}: no KnocFS here (run: knocfs.py format {path})")
        self.bitmap = bytearray(self.read(self.bitmap_start, self.bitmap_blocks))

    # blocks
    def read(self, block, count=1):
        self.file.seek(START_SECTOR * SECTOR + block * BLOCK)
        return self.file.read(count * BLOCK)

    def write(self, block, data):
        self.file.seek(START_SECTOR * SECTOR + block * BLOCK)
        self.file.write(data)

    def close(self):
        self.write(self.bitmap_start, bytes(self.bitmap))
        self.file.close()

    # bitmap
    def used(self, block):
        return (self.bitmap[block // 8] >> (block % 8)) & 1

    def mark(self, block, used):
        if used:
            self.bitmap[block // 8] |= 1 << (block % 8)
        else:
            self.bitmap[block // 8] &= ~(1 << (block % 8)) & 0xFF

    def alloc_run(self, want):
        best, start, count = (0, 0), 0, 0
        for block in range(self.data_start, self.total_blocks):
            if self.used(block):
                count = 0
                continue
            if count == 0:
                start = block
            count += 1
            if count > best[1]:
                best = (start, count)
            if count == want:
                break
        if best[1] == 0:
            sys.exit("KnocFS is full")
        for block in range(best[0], best[0] + best[1]):
            self.mark(block, 1)
        return best

    # inodes
    def inode(self, number):
        data = self.read(self.inode_start + number * INODE.size // BLOCK)
        offset = (number * INODE.size) % BLOCK
        fields = INODE.unpack_from(data, offset)
        extents = [(fields[4 + 2 * i], fields[5 + 2 * i]) for i in range(fields[2])]
        return {"type": fields[0], "size": fields[3], "extents": extents,
                "created": fields[-2], "modified": fields[-1]}

    def put_inode(self, number, inode):
        block = self.inode_start + number * INODE.size // BLOCK
        data = bytearray(self.read(block))
        extents = inode["extents"] + [(0, 0)] * (EXTENTS - len(inode["extents"]))
        flat = [value for extent in extents for value in extent]
        INODE.pack_into(data, (number * INODE.size) % BLOCK,
                        inode["type"], 0, len(inode["extents"]), inode["size"], *flat,
                        int(inode.get("created", 0)), int(inode.get("modified", 0)))
        self.write(block, bytes(data))

    def alloc_inode(self):
        for number in range(2, self.inode_count):
            if self.inode(number)["type"] == TYPE_FREE:
                return number
        sys.exit("KnocFS has no free inodes")

    # file data
    def read_file(self, number):
        inode = self.inode(number)
        data = b"".join(self.read(start, count) for start, count in inode["extents"])
        return data[:inode["size"]]

    def free_data(self, inode):
        for start, count in inode["extents"]:
            for block in range(start, start + count):
                self.mark(block, 0)
        inode["extents"], inode["size"] = [], 0

    def write_file(self, number, data):
        inode = self.inode(number)
        self.free_data(inode)
        needed = (len(data) + BLOCK - 1) // BLOCK
        while needed > 0:
            if len(inode["extents"]) == EXTENTS:
                sys.exit("file too fragmented (more than 12 extents)")
            start, count = self.alloc_run(needed)
            inode["extents"].append((start, count))
            needed -= count
        position = 0
        for start, count in inode["extents"]:
            chunk = data[position:position + count * BLOCK]
            self.write(start, chunk.ljust(count * BLOCK, b"\0"))
            position += count * BLOCK
        inode["size"] = len(data)
        inode["modified"] = int(time.time())
        self.put_inode(number, inode)

    # directories
    def entries(self, directory):
        data = self.read_file(directory)
        for index in range(len(data) // DIRENT.size):
            number, raw = DIRENT.unpack_from(data, index * DIRENT.size)
            yield index, number, raw.split(b"\0")[0].decode()

    def lookup(self, path):
        number = ROOT
        for name in [part for part in path.split("/") if part]:
            if self.inode(number)["type"] != TYPE_DIR:
                return None
            found = [n for _, n, entry in self.entries(number) if n and entry == name]
            if not found:
                return None
            number = found[0]
        return number

    def create(self, path, kind):
        parent_path, name = path.rstrip("/").rsplit("/", 1)
        parent = self.lookup(parent_path or "/")
        if parent is None or self.inode(parent)["type"] != TYPE_DIR:
            sys.exit(f"{parent_path or '/'}: no such directory")
        if len(name.encode()) >= NAME_MAX or not name:
            sys.exit(f"{name}: bad name")
        if self.lookup(path) is not None:
            sys.exit(f"{path}: already exists")
        number = self.alloc_inode()
        now = int(time.time())
        self.put_inode(number, {"type": kind, "size": 0, "extents": [], "created": now, "modified": now})
        data = bytearray(self.read_file(parent))
        entry = DIRENT.pack(number, name.encode())
        for index, entry_number, _ in self.entries(parent):
            if entry_number == 0:
                data[index * DIRENT.size:(index + 1) * DIRENT.size] = entry
                break
        else:
            data += entry
        self.write_file(parent, bytes(data))
        return number

    def remove(self, path):
        number = self.lookup(path)
        if number is None or number == ROOT:
            sys.exit(f"{path}: cannot remove")
        inode = self.inode(number)
        if inode["type"] == TYPE_DIR and any(n for _, n, _ in self.entries(number)):
            sys.exit(f"{path}: directory not empty")
        self.free_data(inode)
        inode["type"] = TYPE_FREE
        self.put_inode(number, inode)
        parent_path = path.rstrip("/").rsplit("/", 1)[0] or "/"
        parent = self.lookup(parent_path)
        data = bytearray(self.read_file(parent))
        for index, entry_number, _ in self.entries(parent):
            if entry_number == number:
                data[index * DIRENT.size:(index + 1) * DIRENT.size] = bytes(DIRENT.size)
        self.write_file(parent, bytes(data))

    def put(self, path, data, modified=None):
        if self.lookup(path) is not None:
            self.remove(path)
        number = self.create(path, TYPE_FILE)
        self.write_file(number, data)
        if modified is not None:
            self.touch(path, modified)

    def link(self, target, path):
        if self.lookup(path) is not None:
            self.remove(path)
        self.write_file(self.create(path, TYPE_LINK), target.encode())

    def put_tree(self, source, dest):
        for directory, folders, files in os.walk(source, followlinks=False):
            relative = os.path.relpath(directory, source)
            base = dest.rstrip("/") + ("" if relative == "." else "/" + relative)
            if base and self.lookup(base) is None:
                self.create(base, TYPE_DIR)
            for name in sorted(folders + files):
                host = os.path.join(directory, name)
                path = (base or "") + "/" + name
                if os.path.islink(host):
                    self.link(os.readlink(host), path)
                    if name in folders:
                        folders.remove(name)
                elif os.path.isfile(host):
                    with open(host, "rb") as f:
                        self.put(path, f.read(), os.path.getmtime(host))

    def touch(self, path, modified):
        number = self.lookup(path)
        if number is None:
            sys.exit(f"{path}: no such file")
        inode = self.inode(number)
        inode["modified"] = int(modified)
        inode["created"] = min(int(inode["created"] or modified), int(modified))
        self.put_inode(number, inode)


def format_image(path):
    size = os.path.getsize(path)
    sectors = size // SECTOR
    total_blocks = (sectors - BLACKBOX_SECTORS - START_SECTOR) // (BLOCK // SECTOR)
    if total_blocks < 64:
        sys.exit(f"{path}: too small for KnocFS (at least 2 MiB)")
    bitmap_blocks = (total_blocks + BLOCK * 8 - 1) // (BLOCK * 8)
    inode_blocks = INODE_COUNT * INODE.size // BLOCK
    bitmap_start = 1
    inode_start = bitmap_start + bitmap_blocks
    data_start = inode_start + inode_blocks

    with open(path, "r+b") as file:
        base = START_SECTOR * SECTOR
        file.seek(base)
        file.write(bytes(BLOCK * data_start))
        bitmap = bytearray(bitmap_blocks * BLOCK)
        for block in range(data_start):
            bitmap[block // 8] |= 1 << (block % 8)
        file.seek(base + bitmap_start * BLOCK)
        file.write(bitmap)
        root = bytearray(INODE.size)
        INODE.pack_into(root, 0, TYPE_DIR, 0, 0, 0, *([0] * (2 * EXTENTS)), 0, 0)
        file.seek(base + inode_start * BLOCK + ROOT * INODE.size)
        file.write(root)
        file.seek(base)
        file.write(SUPER.pack(MAGIC, VERSION, BLOCK, START_SECTOR, total_blocks, INODE_COUNT,
                              bitmap_start, bitmap_blocks, inode_start, inode_blocks, data_start, 0))
    print(f"KnocFS: {total_blocks * BLOCK // (1024 * 1024)} MiB, {INODE_COUNT} inodes")


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    command, image, args = argv[1], argv[2], argv[3:]

    if command == "format":
        format_image(image)
        return

    fs = KnocFS(image)
    try:
        if command == "mkdir":
            for path in args:
                if fs.lookup(path) is None:
                    fs.create(path, TYPE_DIR)
        elif command == "put":
            with open(args[0], "rb") as source:
                fs.put(args[1], source.read(), os.path.getmtime(args[0]))
        elif command == "put-many":
            if fs.lookup(args[0]) is None:
                fs.create(args[0], TYPE_DIR)
            for source_path in args[1:]:
                with open(source_path, "rb") as source:
                    fs.put(args[0].rstrip("/") + "/" + os.path.basename(source_path), source.read(),
                           os.path.getmtime(source_path))
        elif command == "put-text":
            fs.put(args[0], args[1].encode())
        elif command == "make-test-model":
            size = int(args[1]) * 1024 * 1024
            data = bytearray(size)
            for i in range(0, size):
                data[i] = test_model_byte(i)
            fs.put(args[0], bytes(data))
        elif command == "ls":
            number = fs.lookup(args[0] if args else "/")
            if number is None:
                sys.exit("no such file or directory")
            for _, entry, name in fs.entries(number):
                if entry:
                    inode = fs.inode(entry)
                    kind = "dir " if inode["type"] == TYPE_DIR else "link" if inode["type"] == TYPE_LINK else "file"
                    when = time.strftime("%Y-%m-%d %H:%M", time.gmtime(inode["modified"])) if inode["modified"] else "-"
                    print(f"{kind} {inode['size']:>12}  {when}  {name}")
        elif command == "put-link":
            fs.link(args[0], args[1])
        elif command == "put-tree":
            fs.put_tree(args[0], args[1] if len(args) > 1 else "/")
        elif command == "touch":
            fs.touch(args[0], int(args[1]))
        elif command == "cat":
            number = fs.lookup(args[0])
            if number is None:
                sys.exit("no such file")
            sys.stdout.buffer.write(fs.read_file(number))
        elif command == "rm":
            fs.remove(args[0])
        elif command == "info":
            free = sum(1 for b in range(fs.data_start, fs.total_blocks) if not fs.used(b))
            print(f"KnocFS: {fs.total_blocks} blocks of 4 KiB, {free * BLOCK // 1024} KiB free")
        else:
            sys.exit(__doc__)
    finally:
        fs.close()


if __name__ == "__main__":
    main(sys.argv)
