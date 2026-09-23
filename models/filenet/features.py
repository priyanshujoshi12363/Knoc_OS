import math

HEAD_BYTES = 512
HIST = 256
PAIRS = 512
MAGIC = 256
NAME = 256
EXT = 64
SHAPE = 8
FEATURES = HIST + PAIRS + MAGIC + NAME + EXT + SHAPE

MAGIC_POSITIONS = list(range(32)) + list(range(257, 263))

TYPES = ["image", "audio", "video", "document", "text",
         "code", "program", "archive", "model", "data"]
SOURCES = ["whatsapp", "telegram", "camera", "screenshot", "screen_recording", "other"]


def fnv1a(data):
    value = 0x811C9DC5
    for byte in data:
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def extension_of(name):
    lower = name.lower()
    if "." not in lower:
        return ""
    return lower.rsplit(".", 1)[1][:8]


def printable_fraction(head):
    if not head:
        return 0.0
    count = sum(1 for b in head if 32 <= b < 127 or b in (9, 10, 13) or b >= 0xC2)
    return count / len(head)


def features(name, size, head):
    head = head[:HEAD_BYTES]
    vector = [0.0] * FEATURES

    counts = [0] * 256
    for b in head:
        counts[b] += 1
    for b in range(256):
        vector[b] = min(counts[b], 32) / 32

    offset = HIST
    pairs = [0] * PAIRS
    for i in range(len(head) - 1):
        pairs[fnv1a(head[i:i + 2]) % PAIRS] += 1
    for i in range(PAIRS):
        vector[offset + i] = min(pairs[i], 16) / 16

    offset += PAIRS
    for position in MAGIC_POSITIONS:
        if position < len(head):
            key = bytes([position & 0xFF, position >> 8, head[position]])
            vector[offset + fnv1a(key) % MAGIC] = 1.0

    offset += MAGIC
    lower = ("^" + name.lower() + "$").encode("utf-8", errors="replace")
    trigrams = [0] * NAME
    for i in range(len(lower) - 2):
        trigrams[fnv1a(lower[i:i + 3]) % NAME] += 1
    for i in range(NAME):
        vector[offset + i] = min(trigrams[i], 4) / 4

    offset += NAME
    extension = extension_of(name)
    if extension:
        vector[offset + fnv1a(extension.encode()) % EXT] = 1.0

    offset += EXT
    letters = max(len(name), 1)
    vector[offset + 0] = 1.0 if extension else 0.0
    vector[offset + 1] = min(len(name), 64) / 64
    vector[offset + 2] = sum(c.isdigit() for c in name) / letters
    vector[offset + 3] = sum(c.isupper() for c in name) / letters
    vector[offset + 4] = min(name.count(" "), 8) / 8
    vector[offset + 5] = min(math.log2(size + 1), 40) / 40
    vector[offset + 6] = printable_fraction(head)
    vector[offset + 7] = len(head) / HEAD_BYTES
    return vector
