#!/usr/bin/env python3
import sys


def load(path):
    data = open(path, "rb").read()
    parts = []
    index = 0
    while len(parts) < 4:
        while data[index:index + 1].isspace():
            index += 1
        if data[index:index + 1] == b"#":
            while data[index:index + 1] != b"\n":
                index += 1
            continue
        start = index
        while not data[index:index + 1].isspace():
            index += 1
        parts.append(data[start:index])
    width, height = int(parts[1]), int(parts[2])
    return width, height, data[index + 1:]


def pixel(image, x, y):
    width, _, body = image
    offset = (y * width + x) * 3
    return body[offset], body[offset + 1], body[offset + 2]


def main():
    image = load(sys.argv[1])
    failed = 0
    for check in sys.argv[2:]:
        kind, _, rest = check.partition(":")
        if kind == "is":
            where, _, color = rest.partition("=")
            x, y = map(int, where.split(","))
            want = tuple(int(color[i:i + 2], 16) for i in (0, 2, 4))
            got = pixel(image, x, y)
            ok = all(abs(a - b) <= 3 for a, b in zip(got, want))
        elif kind == "not":
            where, _, color = rest.partition("=")
            x, y = map(int, where.split(","))
            want = tuple(int(color[i:i + 2], 16) for i in (0, 2, 4))
            got = pixel(image, x, y)
            ok = any(abs(a - b) > 12 for a, b in zip(got, want))
        elif kind == "bright":
            box, _, minimum = rest.partition(">")
            x, y, w, h = map(int, box.split(","))
            count = sum(1 for j in range(y, y + h) for i in range(x, x + w) if sum(pixel(image, i, j)) > 450)
            got = count
            ok = count > int(minimum)
        else:
            print("  MISS unknown pixel check " + check)
            failed = 1
            continue
        print(("  ok   " if ok else "  MISS ") + "screen " + check + ("" if ok else f" (got {got})"))
        failed |= not ok
    sys.exit(failed)


main()
