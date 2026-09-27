MAX_WORDS = 512
MAX_WORD = 32
MIN_N = 3
MAX_N = 5


def fnv1a(data):
    value = 0x811C9DC5
    for byte in data:
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def words(text):
    data = text.encode("utf-8", "ignore") if isinstance(text, str) else text
    out = []
    current = bytearray()
    previous = 0

    def flush():
        if current:
            out.append(bytes(current[:MAX_WORD]))
            current.clear()

    for byte in data:
        if 65 <= byte <= 90:
            kind, value = 1, byte + 32
        elif 97 <= byte <= 122 or byte >= 128:
            kind, value = 2, byte
        elif 48 <= byte <= 57:
            kind, value = 3, byte
        else:
            flush()
            previous = 0
            continue
        if current and ((previous == 3) != (kind == 3) or (previous == 2 and kind == 1)):
            flush()
        current.append(value)
        previous = kind
        if len(out) >= MAX_WORDS:
            break
    flush()
    return out[:MAX_WORDS]


def features(word, buckets):
    ids = [fnv1a(b"W" + word) % buckets]
    if word.isdigit():
        return ids
    wrapped = b"<" + word + b">"
    for n in range(MIN_N, MAX_N + 1):
        for i in range(0, len(wrapped) - n + 1):
            ids.append(fnv1a(b"C" + wrapped[i:i + n]) % buckets)
    return ids
