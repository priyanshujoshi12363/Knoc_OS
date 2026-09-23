#!/usr/bin/env python3
import argparse
import csv
import os
import random
import struct
from collections import Counter, defaultdict

HEAD_BYTES = 512
CLASSES = ["image", "audio", "video", "document", "text",
           "code", "program", "archive", "model", "data"]
SOURCES = ["whatsapp", "telegram", "camera", "screenshot", "screen_recording", "other"]

SOURCES_FOR = {
    "image": {"whatsapp": 3, "telegram": 2, "camera": 3, "screenshot": 3, "other": 2},
    "video": {"whatsapp": 3, "telegram": 2, "camera": 3, "screen_recording": 2, "other": 1},
    "audio": {"whatsapp": 3, "telegram": 2, "other": 3},
    "document": {"whatsapp": 2, "telegram": 2, "other": 5},
    "text": {"other": 1},
    "code": {"other": 1},
    "archive": {"other": 3, "telegram": 1, "whatsapp": 1},
    "model": {"other": 1},
    "data": {"other": 1},
    "program": {"other": 1},
}

SOURCE_DIRS = ["/usr/share", "/usr/lib", "/usr/bin", "/usr/include"]

EXTENSIONS = {
    "png": "image", "jpg": "image", "jpeg": "image", "gif": "image",
    "bmp": "image", "ico": "image", "webp": "image", "svg": "image",
    "wav": "audio", "ogg": "audio", "oga": "audio", "mp3": "audio", "flac": "audio",
    "webm": "video", "mp4": "video", "mkv": "video", "avi": "video",
    "pdf": "document", "html": "document", "htm": "document",
    "txt": "text", "md": "text", "rst": "text", "conf": "text", "json": "text",
    "xml": "text", "desktop": "text", "csv": "text", "ini": "text", "cfg": "text",
    "py": "code", "js": "code", "c": "code", "h": "code", "hpp": "code",
    "cpp": "code", "sh": "code", "pl": "code", "pm": "code", "css": "code",
    "rs": "code", "go": "code", "java": "code", "rb": "code", "lua": "code",
    "so": "program", "o": "program", "pyc": "program",
    "gz": "archive", "zst": "archive", "zip": "archive", "jar": "archive",
    "xz": "archive", "bz2": "archive", "whl": "archive", "tar": "archive",
    "ttf": "data", "otf": "data", "mo": "data", "dat": "data", "typelib": "data",
    "db": "data", "sqlite": "data", "bin": "data",
}

MAGIC = {
    "png": [b"\x89PNG"], "jpg": [b"\xff\xd8\xff"], "jpeg": [b"\xff\xd8\xff"],
    "gif": [b"GIF8"], "bmp": [b"BM"], "ico": [b"\x00\x00\x01\x00"],
    "wav": [b"RIFF"], "ogg": [b"OggS"], "oga": [b"OggS"], "flac": [b"fLaC"],
    "mp3": [b"ID3", b"\xff\xfb", b"\xff\xf3"], "webm": [b"\x1a\x45\xdf\xa3"],
    "pdf": [b"%PDF"], "so": [b"\x7fELF"], "o": [b"\x7fELF"],
    "gz": [b"\x1f\x8b"], "zst": [b"\x28\xb5\x2f\xfd"], "zip": [b"PK\x03\x04"],
    "jar": [b"PK\x03\x04"], "whl": [b"PK\x03\x04"], "xz": [b"\xfd7zXZ\x00"],
    "bz2": [b"BZh"], "ttf": [b"\x00\x01\x00\x00", b"true", b"OTTO"],
    "otf": [b"OTTO", b"\x00\x01\x00\x00"], "mo": [b"\xde\x12\x04\x95", b"\x95\x04\x12\xde"],
}

TEXT_CLASSES = {"text", "code", "document"}
TEXT_EXTENSIONS = {"svg", "html", "htm"}

WRONG_EXTENSIONS = ["txt", "bin", "dat", "png", "zip", "py", "pdf", "mp4", "doc", "tmp"]


def looks_like_text(head):
    text = None
    for cut in range(4):
        try:
            text = head[:len(head) - cut].decode("utf-8")
            break
        except UnicodeDecodeError:
            continue
    if not text:
        return False
    printable = sum(ch.isprintable() or ch in "\n\r\t" for ch in text)
    return printable / len(text) > 0.95


def valid(extension, head):
    if extension in MAGIC:
        return any(head.startswith(magic) for magic in MAGIC[extension])
    if extension == "pyc":
        return len(head) >= 16
    if extension == "svg":
        return b"<svg" in head.lower() and looks_like_text(head)
    if EXTENSIONS[extension] in TEXT_CLASSES or extension in TEXT_EXTENSIONS:
        return looks_like_text(head)
    return len(head) > 0


def collect_real(per_class, per_extension, rng):
    found = defaultdict(list)

    for root_dir in SOURCE_DIRS:
        for directory, subdirs, names in os.walk(root_dir, followlinks=False):
            subdirs.sort()
            for name in sorted(names):
                path = os.path.join(directory, name)
                if os.path.islink(path):
                    continue
                if "." in name:
                    extension = name.rsplit(".", 1)[1].lower()
                elif directory.startswith("/usr/bin"):
                    extension = "__exec__"
                else:
                    continue
                if extension != "__exec__" and extension not in EXTENSIONS:
                    continue
                found[extension].append(path)

    samples = []
    for extension, paths in sorted(found.items()):
        rng.shuffle(paths)
        kept = 0
        for path in paths:
            if kept >= per_extension:
                break
            try:
                size = os.path.getsize(path)
                with open(path, "rb") as file:
                    head = file.read(HEAD_BYTES)
            except OSError:
                continue
            if size < 16:
                continue
            if extension == "__exec__":
                if not head.startswith(b"\x7fELF"):
                    continue
                label = "program"
            else:
                if not valid(extension, head):
                    continue
                label = EXTENSIONS[extension]
            samples.append({"label": label, "source": "other", "name": os.path.basename(path),
                            "size": size, "origin": "real", "head": head})
            kept += 1

    by_class = defaultdict(list)
    for sample in samples:
        by_class[sample["label"]].append(sample)
    result = []
    for label, items in by_class.items():
        rng.shuffle(items)
        result.extend(items[:per_class])
    return result


def random_bytes(rng, count):
    return bytes(rng.getrandbits(8) for _ in range(count))


def pad(rng, head):
    return (head + random_bytes(rng, HEAD_BYTES))[:HEAD_BYTES]


def words(rng, count):
    vocabulary = ["the", "kernel", "model", "file", "data", "system", "memory",
                  "process", "network", "value", "result", "time", "user", "list",
                  "open", "read", "write", "config", "table", "report", "image"]
    return " ".join(rng.choice(vocabulary) for _ in range(count))


def make_mp4(rng):
    brand = rng.choice([b"isom", b"mp42", b"avc1", b"M4V "])
    ftyp = struct.pack(">I", 24) + b"ftyp" + brand + b"\x00\x00\x02\x00" + b"isomiso2"
    moov = struct.pack(">I", rng.randint(1000, 90000)) + rng.choice([b"moov", b"mdat", b"free"])
    return pad(rng, ftyp + moov + b"\x00\x00\x00\x6cmvhd"), rng.choice(["mp4", "m4v", "mov"])


def make_mkv(rng):
    doc_type = rng.choice([b"matroska", b"webm"])
    header = (b"\x1a\x45\xdf\xa3\x9f\x42\x86\x81\x01\x42\xf7\x81\x01\x42\xf2\x81\x04"
              b"\x42\xf3\x81\x08\x42\x82" + bytes([0x80 | len(doc_type)]) + doc_type
              + b"\x42\x87\x81\x04\x42\x85\x81\x02\x18\x53\x80\x67")
    return pad(rng, header), "webm" if doc_type == b"webm" else "mkv"


def make_avi(rng):
    header = (b"RIFF" + struct.pack("<I", rng.randint(10**5, 10**9)) + b"AVI LIST"
              + struct.pack("<I", rng.randint(1000, 9000)) + b"hdrlavih")
    return pad(rng, header), "avi"


def make_mp3(rng):
    if rng.random() < 0.6:
        header = b"ID3" + bytes([3, 0, 0]) + random_bytes(rng, 4) + b"TIT2"
    else:
        header = rng.choice([b"\xff\xfb\x90\x64", b"\xff\xfb\x92\x04", b"\xff\xf3\x84\xc4"])
    return pad(rng, header), "mp3"


def make_flac(rng):
    header = b"fLaC\x00\x00\x00\x22" + struct.pack(">HH", 4096, 4096) + random_bytes(rng, 6)
    return pad(rng, header), "flac"


def make_wav(rng):
    header = (b"RIFF" + struct.pack("<I", rng.randint(10**4, 10**8)) + b"WAVEfmt "
              + struct.pack("<IHHIIHH", 16, 1, rng.choice([1, 2]),
                            rng.choice([22050, 44100, 48000]), 176400, 4, 16) + b"data")
    return pad(rng, header), "wav"


PDF_OBJECTS = [
    ["/Type /Catalog", "/Pages 2 0 R"],
    ["/Type /Pages", "/Kids [3 0 R]", "/Count {count}"],
    ["/Type /Page", "/Parent 2 0 R", "/MediaBox [0 0 612 792]", "/Contents 4 0 R"],
    ["/Type /ExtGState", "/ca 0.{alpha}", "/CA 0.{alpha}"],
    ["/Type /Font", "/Subtype /Type1", "/BaseFont /Helvetica"],
    ["/Type /XObject", "/Subtype /Image", "/Width {width}", "/Height {height}"],
    ["/Linearized 1", "/L {length}", "/O {page}", "/N {count}"],
    ["/Producer ({producer})", "/Creator ({producer})", "/CreationDate (D:2024{month}01120000)"],
    ["/Length {length}", "/Filter /FlateDecode"],
]
PDF_PRODUCERS = ["Microsoft Word", "LibreOffice", "Skia/PDF", "iText", "ReportLab", "wkhtmltopdf",
                 "Adobe PDF Library", "Ghostscript", "Chromium", "FPDF"]


def make_pdf(rng):
    version = rng.choice(["1.3", "1.4", "1.5", "1.6", "1.7", "2.0"])
    newline = rng.choice(["\n", "\n", "\r\n", "\r"])
    multiline = rng.random() < 0.5
    parts = [f"%PDF-{version}"]
    if rng.random() < 0.85:
        parts.append("%\xe2\xe3\xcf\xd3")
    values = {"count": rng.randint(1, 300), "alpha": rng.randint(1, 9), "width": rng.randint(64, 3000),
              "height": rng.randint(64, 3000), "length": rng.randint(1000, 900000),
              "page": rng.randint(3, 99), "producer": rng.choice(PDF_PRODUCERS),
              "month": f"{rng.randint(1, 12):02}"}
    for _ in range(rng.randint(2, 5)):
        keys = [key.format(**values) for key in rng.choice(PDF_OBJECTS)]
        number = rng.randint(1, 60)
        if multiline:
            body = newline.join(["<<"] + keys + [">>"])
        else:
            body = "<< " + " ".join(keys) + " >>"
        parts.append(f"{number} 0 obj{newline}{body}{newline}endobj")
    return pad(rng, newline.join(parts).encode("latin-1")), "pdf"


def make_office(rng):
    kind = rng.choice(["docx", "odt", "epub"])
    if kind == "docx":
        inner = b"[Content_Types].xml"
    elif kind == "odt":
        inner = b"mimetypeapplication/vnd.oasis.opendocument.text"
    else:
        inner = b"mimetypeapplication/epub+zip"
    header = (b"PK\x03\x04\x14\x00\x00\x00\x08\x00" + random_bytes(rng, 16)
              + struct.pack("<HH", len(inner), 0) + inner)
    return pad(rng, header), kind


def make_html(rng):
    title = words(rng, 3)
    body = (f"<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
            f"<title>{title}</title>\n</head>\n<body>\n<h1>{title}</h1>\n<p>{words(rng, 40)}</p>\n")
    return body.encode()[:HEAD_BYTES], "html"


def make_csv(rng):
    columns = rng.sample(["id", "name", "time", "value", "score", "size", "label", "count"], 4)
    lines = [",".join(columns)]
    for i in range(30):
        lines.append(",".join(str(rng.randint(0, 9999)) if c != "name" else words(rng, 1)
                              for c in columns))
    return "\n".join(lines).encode()[:HEAD_BYTES], "csv"


def make_tar(rng):
    name = (words(rng, 1) + "/" + words(rng, 1) + ".txt").encode()
    header = bytearray(HEAD_BYTES)
    header[0:len(name)] = name
    header[100:108] = b"0000644\x00"
    header[124:136] = f"{rng.randint(1, 10**7):011o}\x00".encode()
    header[156] = ord("0")
    header[257:265] = b"ustar\x0000"
    return bytes(header), "tar"


def make_compressed(rng):
    kind = rng.choice(["gz", "bz2", "xz", "zst"])
    if kind == "gz":
        header = b"\x1f\x8b\x08" + bytes([rng.choice([0, 8])]) + rng.randbytes(6)
    elif kind == "bz2":
        header = b"BZh" + bytes([ord("1") + rng.randrange(9)]) + b"1AY&SY"
    elif kind == "xz":
        header = b"\xfd7zXZ\x00\x00" + bytes([rng.choice([1, 4])])
    else:
        header = b"\x28\xb5\x2f\xfd" + bytes([rng.choice([0x24, 0x64, 0xa4])])
    extension = rng.choice(["tar." + kind, "tar." + kind, kind])
    return pad(rng, header), extension


def make_7z(rng):
    return pad(rng, b"7z\xbc\xaf\x27\x1c\x00\x04"), "7z"


def make_gguf(rng):
    version = rng.choice([2, 3])
    header = (b"GGUF" + struct.pack("<IQQ", version, rng.randint(100, 900), rng.randint(15, 40)))
    key = b"general.architecture"
    header += struct.pack("<Q", len(key)) + key + struct.pack("<I", 8)
    arch = rng.choice([b"llama", b"qwen2", b"gemma", b"phi3"])
    header += struct.pack("<Q", len(arch)) + arch
    return pad(rng, header), "gguf"


def make_safetensors(rng):
    tensors = ", ".join(
        f'"model.layers.{i}.weight": {{"dtype": "{rng.choice(["F16", "BF16", "F32"])}", '
        f'"shape": [{rng.choice([896, 1024, 2048])}, {rng.choice([896, 1024, 4864])}], '
        f'"data_offsets": [{i * 1000}, {(i + 1) * 1000}]}}' for i in range(4))
    text = ("{" + tensors + "}").encode()
    return (struct.pack("<Q", len(text) + rng.randint(0, 5000)) + text)[:HEAD_BYTES], "safetensors"


def make_onnx(rng):
    producer = rng.choice([b"pytorch", b"tf2onnx", b"onnxruntime"])
    header = b"\x08" + bytes([rng.randint(3, 9)]) + b"\x12" + bytes([len(producer)]) + producer
    header += b"\x1a\x05" + b"2.1.0" + b"\x3a" + random_bytes(rng, 8) + b"\x0a\x04Gemm"
    return pad(rng, header), "onnx"


def make_knm(rng):
    header = b"KNOCNN01" + struct.pack("<IIII", 1, rng.randint(2, 6), rng.randint(16, 512),
                                        rng.randint(2, 16))
    return pad(rng, header), "knm"


def make_sqlite(rng):
    return pad(rng, b"SQLite format 3\x00" + struct.pack(">H", 4096)), "db"


def make_jpeg(rng, source):
    if source == "camera":
        maker = rng.choice([b"samsung", b"Google", b"Apple", b"Xiaomi", b"OnePlus", b"Canon"])
        model = rng.choice([b"SM-S918B", b"Pixel 8", b"iPhone 14", b"Redmi Note 12", b"EOS 90D"])
        exif = (b"Exif\x00\x00MM\x00\x2a\x00\x00\x00\x08\x00\x0c\x01\x0f\x00\x02"
                + random_bytes(rng, 8) + maker + b"\x00" + model + b"\x00")
        header = b"\xff\xd8\xff\xe1" + struct.pack(">H", len(exif) + 2) + exif
    else:
        header = (b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"
                  b"\xff\xdb\x00\x43\x00")
    return pad(rng, header), rng.choice(["jpg", "jpg", "jpeg"])


def make_png(rng, source):
    width = rng.choice([1080, 1170, 1440, 1920, 2560, 800]) if source == "screenshot" \
        else rng.randint(16, 4000)
    height = rng.choice([2340, 2532, 3120, 1080, 1440, 600]) if source == "screenshot" \
        else rng.randint(16, 4000)
    header = (b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR" + struct.pack(">II", width, height)
              + bytes([8, rng.choice([2, 6]), 0, 0, 0]) + random_bytes(rng, 4))
    if rng.random() < 0.5:
        software = rng.choice([b"gnome-screenshot", b"Android", b"Screenshot", b"Adobe ImageReady"])
        header += struct.pack(">I", len(software) + 9) + b"tEXtSoftware\x00" + software
    return pad(rng, header), "png"


def make_heic(rng, source):
    header = struct.pack(">I", 24) + b"ftypheic" + b"\x00\x00\x00\x00mif1heic"
    return pad(rng, header + b"\x00\x00\x01\x00meta"), rng.choice(["HEIC", "heic"])


def make_webp(rng, source):
    header = b"RIFF" + struct.pack("<I", rng.randint(10**3, 10**6)) + b"WEBPVP8 "
    return pad(rng, header), "webp"


def make_gif(rng, source):
    header = b"GIF89a" + struct.pack("<HH", rng.randint(16, 800), rng.randint(16, 800))
    return pad(rng, header), "gif"


def make_image(rng, source):
    if source == "screenshot":
        return make_png(rng, source) if rng.random() < 0.85 else make_jpeg(rng, source)
    if source == "camera":
        return make_heic(rng, source) if rng.random() < 0.2 else make_jpeg(rng, source)
    if source in ("whatsapp", "telegram"):
        return make_jpeg(rng, source) if rng.random() < 0.85 else make_webp(rng, source)
    return rng.choice([make_jpeg, make_png, make_webp, make_gif])(rng, source)


def make_mov(rng):
    header = struct.pack(">I", 20) + b"ftypqt  " + b"\x00\x00\x02\x00qt  "
    return pad(rng, header + b"\x00\x00\x00\x08wide"), "mov"


def make_video(rng, source):
    if source == "screen_recording":
        return rng.choice([make_mov, make_mp4, make_mkv])(rng)
    if source == "camera":
        return make_mov(rng) if rng.random() < 0.3 else make_mp4(rng)
    if source in ("whatsapp", "telegram"):
        return make_mp4(rng)
    return rng.choice([make_mp4, make_mkv, make_avi])(rng)


def make_opus(rng):
    header = (b"OggS\x00\x02" + random_bytes(rng, 20) + b"\x01\x13OpusHead\x01\x01"
              + struct.pack("<HI", 312, 48000))
    return pad(rng, header), "opus"


def make_m4a(rng):
    header = struct.pack(">I", 28) + b"ftypM4A " + b"\x00\x00\x02\x00M4A mp42isom"
    return pad(rng, header), "m4a"


def make_audio(rng, source):
    if source == "whatsapp":
        return make_opus(rng) if rng.random() < 0.7 else make_m4a(rng)
    if source == "telegram":
        return make_opus(rng) if rng.random() < 0.6 else make_mp3(rng)
    return rng.choice([make_mp3, make_flac, make_wav, make_m4a])(rng)


def make_zip(rng):
    inner = (words(rng, 1) + "/" + words(rng, 1) + ".txt").encode()
    header = (b"PK\x03\x04\x14\x00\x00\x00\x08\x00" + random_bytes(rng, 16)
              + struct.pack("<HH", len(inner), 0) + inner)
    return pad(rng, header), "zip"


def make_rar(rng):
    header = rng.choice([b"Rar!\x1a\x07\x01\x00", b"Rar!\x1a\x07\x00"]) + random_bytes(rng, 8)
    return pad(rng, header), "rar"


def make_note(rng):
    title = words(rng, 3)
    lines = [f"# {title}", "", f"- {words(rng, 6)}", f"- {words(rng, 5)}", "",
             words(rng, 30)]
    return "\n".join(lines).encode()[:HEAD_BYTES], rng.choice(["txt", "md", "md"])


def make_json(rng):
    body = "{\n" + ",\n".join(f'  "{words(rng, 1)}": {rng.randint(0, 999)}' for _ in range(12)) + "\n}"
    return body.encode()[:HEAD_BYTES], "json"


def make_token(rng):
    alphabet = rng.choice(["abcdefghijklmnopqrstuvwxyz0123456789",
                           "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"])
    token = "".join(rng.choice(alphabet) for _ in range(rng.randint(16, 120)))
    return token.encode(), rng.choice(["txt", "", "", "key"])


def make_code(rng):
    kind = rng.choice(["c", "py", "js", "sh"])
    name = words(rng, 1)
    if kind == "c":
        body = (f"#include <stdio.h>\n\nstatic int {name}_count = 0;\n\n"
                f"int {name}(int value)\n{{\n    return value * {rng.randint(2, 9)};\n}}\n\n"
                f"int main(void)\n{{\n    printf(\"%d\\n\", {name}(3));\n    return 0;\n}}\n")
    elif kind == "py":
        body = (f"import os\nimport sys\n\n\ndef {name}(path):\n    with open(path) as f:\n"
                f"        return f.read()\n\n\nif __name__ == \"__main__\":\n"
                f"    print({name}(sys.argv[1]))\n")
    elif kind == "js":
        body = (f"const {name} = require('{words(rng, 1)}');\n\nfunction run(items) {{\n"
                f"  return items.map((x) => x * {rng.randint(2, 9)});\n}}\n\nmodule.exports = {{ run }};\n")
    else:
        body = (f"#!/usr/bin/env bash\nset -e\n\nfor f in *.{words(rng, 1)}; do\n"
                f"    echo \"$f\"\ndone\n")
    return body.encode()[:HEAD_BYTES], kind


def make_elf(rng):
    header = (b"\x7fELF\x02\x01\x01\x00" + bytes(8) + struct.pack("<HHI", 2, rng.choice([62, 243, 183]), 1)
              + struct.pack("<QQQ", rng.randint(0x400000, 0x500000), 64, rng.randint(10**4, 10**6)))
    return pad(rng, header), rng.choice(["", "bin", "AppImage", "run"])


GENERATORS = {
    "image": [make_image],
    "video": [make_video],
    "audio": [make_audio],
    "document": [lambda rng, source: rng.choice([make_pdf, make_pdf, make_office, make_html])(rng)],
    "text": [lambda rng, source: rng.choice([make_csv, make_note, make_json, make_token])(rng)],
    "code": [lambda rng, source: make_code(rng)],
    "archive": [lambda rng, source: rng.choice([make_zip, make_zip, make_rar, make_tar, make_7z,
                                                make_compressed, make_compressed])(rng)],
    "model": [lambda rng, source: rng.choice([make_gguf, make_safetensors, make_onnx, make_knm])(rng)],
    "data": [lambda rng, source: make_sqlite(rng)],
    "program": [lambda rng, source: make_elf(rng)],
}


def random_time(rng):
    return (rng.randint(2019, 2026), rng.randint(1, 12), rng.randint(1, 28),
            rng.randint(0, 23), rng.randint(0, 59), rng.randint(0, 59))


def whatsapp_name(rng, label, extension):
    y, mo, d, h, mi, s = random_time(rng)
    counter = rng.randint(0, 9999)
    if rng.random() < 0.25:
        kind = {"image": "Image", "video": "Video", "audio": "Audio"}.get(label, "Document")
        return f"WhatsApp {kind} {y}-{mo:02}-{d:02} at {h}.{mi:02}.{s:02}.{extension}"
    prefix = {"image": "IMG", "video": "VID", "document": "DOC", "archive": "DOC"}.get(label)
    if label == "audio":
        prefix = "PTT" if extension == "opus" and rng.random() < 0.7 else "AUD"
    return f"{prefix}-{y}{mo:02}{d:02}-WA{counter:04}.{extension}"


def telegram_name(rng, label, extension):
    y, mo, d, h, mi, s = random_time(rng)
    if label in ("image", "video"):
        kind = "photo" if label == "image" else "video"
        return f"{kind}_{y}-{mo:02}-{d:02}_{h:02}-{mi:02}-{s:02}.{extension}"
    if label == "audio":
        return f"audio_{rng.randint(1, 300)}@{d:02}-{mo:02}-{y}_{h:02}-{mi:02}-{s:02}.{extension}"
    return f"file_{rng.randint(0, 500)}.{extension}"


def camera_name(rng, label, extension):
    y, mo, d, h, mi, s = random_time(rng)
    style = rng.random()
    if extension.lower() in ("heic", "mov"):
        return f"IMG_{rng.randint(1, 9999):04}.{extension}"
    if style < 0.45:
        prefix = "IMG" if label == "image" else "VID"
        return f"{prefix}_{y}{mo:02}{d:02}_{h:02}{mi:02}{s:02}.{extension}"
    if style < 0.7:
        return f"PXL_{y}{mo:02}{d:02}_{h:02}{mi:02}{s:02}{rng.randint(0, 999):03}.{extension}"
    if style < 0.85:
        return f"DSC_{rng.randint(1, 9999):04}.{extension.upper()}"
    return f"{y}{mo:02}{d:02}_{h:02}{mi:02}{s:02}.{extension}"


def screenshot_name(rng, label, extension):
    y, mo, d, h, mi, s = random_time(rng)
    style = rng.random()
    if style < 0.4:
        return f"Screenshot_{y}{mo:02}{d:02}-{h:02}{mi:02}{s:02}.{extension}"
    if style < 0.65:
        return f"Screenshot from {y}-{mo:02}-{d:02} {h:02}-{mi:02}-{s:02}.{extension}"
    if style < 0.85:
        return f"Screenshot {y}-{mo:02}-{d:02} at {h}.{mi:02}.{s:02}.{extension}"
    app = rng.choice(["Chrome", "WhatsApp", "Instagram", "YouTube"])
    return f"Screenshot_{y}{mo:02}{d:02}_{h:02}{mi:02}{s:02}_{app}.{extension}"


def screen_recording_name(rng, label, extension):
    y, mo, d, h, mi, s = random_time(rng)
    style = rng.random()
    if style < 0.35:
        return f"Screen Recording {y}-{mo:02}-{d:02} at {h}.{mi:02}.{s:02}.{extension}"
    if style < 0.7:
        return f"Screencast from {y}-{mo:02}-{d:02} {h:02}-{mi:02}-{s:02}.{extension}"
    return f"Screenrecorder-{y}-{mo:02}-{d:02}-{h:02}-{mi:02}-{s:02}-{rng.randint(0, 999)}.{extension}"


BROWSER_WORDS = ["invoice", "report", "setup", "wallpaper", "manual", "resume", "photo",
                 "lecture", "notes", "dataset", "release", "document", "ticket", "slides"]


def browser_name(rng, label, extension):
    if label == "model":
        family = rng.choice(["qwen2.5", "llama-3.2", "gemma-2", "phi-3", "smollm2", "mistral"])
        size = rng.choice(["0.5b", "1.5b", "3b", "7b", "135m", "2b"])
        quant = rng.choice(["q4_k_m", "q8_0", "f16", "q5_k_s"])
        if extension == "gguf":
            return f"{family}-{size}-instruct-{quant}.gguf"
        return f"{rng.choice(['model', 'pytorch_model', family])}.{extension}"
    style = rng.random()
    if style < 0.15:
        stem = str(rng.randint(10**5, 10**11))
        return f"{stem}.{extension}" if extension else stem
    style = rng.random()
    stem = "_".join(rng.sample(BROWSER_WORDS, rng.randint(1, 2)))
    if style < 0.3:
        stem += f"_{rng.choice(['march', 'final', 'v2', '2024', 'new'])}"
    elif style < 0.5:
        stem += f" ({rng.randint(1, 9)})"
    elif style < 0.6:
        stem = rng.choice(["download", "file", "image", "unnamed"])
    elif style < 0.7:
        stem = f"{rng.choice(BROWSER_WORDS)}-{rng.randint(1000000000, 9999999999)}"
    elif style < 0.8:
        stem = f"{rng.choice(BROWSER_WORDS)}-v{rng.randint(1, 9)}.{rng.randint(0, 20)}.{rng.randint(0, 9)}"
    return f"{stem}.{extension}" if extension else stem


USER_WORDS = ["notes", "todo", "project", "plan", "ideas", "main", "homework", "budget",
              "diary", "draft", "my_resume", "test", "script", "data", "experiment"]


def user_name(rng, label, extension):
    stem = rng.choice(USER_WORDS)
    if rng.random() < 0.4:
        stem += "_" + rng.choice(USER_WORDS)
    if rng.random() < 0.3:
        stem += str(rng.randint(1, 20))
    return f"{stem}.{extension}" if extension else stem


NAMERS = {
    "whatsapp": whatsapp_name, "telegram": telegram_name, "camera": camera_name,
    "screenshot": screenshot_name, "screen_recording": screen_recording_name,
    "other": lambda rng, label, extension: (browser_name if rng.random() < 0.6 else user_name)(rng, label, extension),
}


def generate(label, rng, source=None):
    weights = SOURCES_FOR[label]
    if source is None:
        source = rng.choices(list(weights), weights=list(weights.values()))[0]
    head, extension = rng.choice(GENERATORS[label])(rng, source)

    if source == "whatsapp" and label == "image":
        extension = "jpg" if extension != "webp" else extension
    if source == "whatsapp" and label == "video":
        extension = "mp4"

    name = NAMERS[source](rng, label, extension)
    size = max(len(head), int(rng.lognormvariate(14 if label != "model" else 20, 1.5)))
    return {"label": label, "source": source, "name": name, "size": size,
            "origin": "generated", "head": head}


def rename(sample, rng):
    roll = rng.random()
    stem = sample["name"].rsplit(".", 1)[0] if "." in sample["name"] else sample["name"]
    if roll < 0.08:
        sample["name"] = stem
    elif roll < 0.12:
        sample["name"] = stem + "." + rng.choice(WRONG_EXTENSIONS)
    return sample


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--per-class", type=int, default=1500)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "files")
    os.makedirs(out_dir, exist_ok=True)

    samples = collect_real(args.per_class // 2, args.per_class // 6, rng)
    counts = Counter(sample["label"] for sample in samples)

    for label in CLASSES:
        while counts[label] < args.per_class:
            samples.append(generate(label, rng))
            counts[label] += 1

    source_counts = Counter(sample["source"] for sample in samples)
    source_target = args.per_class * 4 // 5
    for source in SOURCES:
        if source == "other":
            continue
        types = {label: weights[source] for label, weights in SOURCES_FOR.items() if source in weights}
        while source_counts[source] < source_target:
            label = rng.choices(list(types), weights=list(types.values()))[0]
            samples.append(generate(label, rng, source))
            source_counts[source] += 1

    samples = [rename(sample, rng) for sample in samples]
    rng.shuffle(samples)

    splits = {"train": [], "val": [], "test": []}
    for i, sample in enumerate(samples):
        part = i % 10
        splits["train" if part < 8 else "val" if part == 8 else "test"].append(sample)

    for split, rows in splits.items():
        with open(os.path.join(out_dir, f"{split}.csv"), "w", newline="") as file:
            writer = csv.writer(file)
            writer.writerow(["label", "source", "name", "size", "origin", "head_hex"])
            for row in rows:
                writer.writerow([row["label"], row["source"], row["name"], row["size"],
                                 row["origin"], row["head"].hex()])

    lines = [f"{'type':<10} {'real':>6} {'generated':>10} {'total':>6}"]
    for label in CLASSES:
        real = sum(1 for s in samples if s["label"] == label and s["origin"] == "real")
        made = sum(1 for s in samples if s["label"] == label and s["origin"] == "generated")
        lines.append(f"{label:<10} {real:>6} {made:>10} {real + made:>6}")
    lines.append("")
    lines.append(f"{'source':<17} {'total':>6}   types")
    for source in SOURCES:
        rows = [s for s in samples if s["source"] == source]
        types = Counter(s["label"] for s in rows)
        lines.append(f"{source:<17} {len(rows):>6}   "
                     + ", ".join(f"{t} {n}" for t, n in types.most_common()))
    lines.append(f"\ntrain {len(splits['train'])}, val {len(splits['val'])}, "
                 f"test {len(splits['test'])}, seed {args.seed}")
    report = "\n".join(lines)

    with open(os.path.join(out_dir, "stats.txt"), "w") as file:
        file.write(report + "\n")
    print(report)


if __name__ == "__main__":
    main()
