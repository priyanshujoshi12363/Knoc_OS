import argparse
import json
import os
import re
import shutil
import sys
import time

import numpy as np

import features as F
import knm

HERE = os.path.dirname(os.path.abspath(__file__))
MODEL = os.path.join(HERE, "filenet.knm")
LOG_PREFIX = ".knocos-organize-"

SOURCE_FOLDERS = {
    "whatsapp": "WhatsApp",
    "telegram": "Telegram",
    "camera": "Camera",
    "screenshot": "Screenshots",
    "screen_recording": "Screen Recordings",
    "other": "",
}

TYPE_FOLDERS = {
    "image": "Images",
    "video": "Videos",
    "audio": "Audio",
    "document": "Documents",
    "text": "Text",
    "code": "Code",
    "program": "Programs",
    "archive": "Archives",
    "model": "AI Models",
    "data": "Data",
}

FLAT_SOURCES = {"screenshot", "screen_recording"}
RANDOM_FOLDER = "Random"

RULE_FOLDERS = {
    "Images": ["jpg", "jpeg", "png", "gif", "bmp", "webp", "heic", "heif", "tif", "tiff", "svg", "ico", "raw", "cr2", "nef"],
    "Videos": ["mp4", "mkv", "webm", "avi", "mov", "m4v", "3gp", "flv", "wmv", "mpeg", "mpg", "ts"],
    "Audio": ["mp3", "wav", "flac", "ogg", "oga", "opus", "m4a", "aac", "amr", "wma", "mid", "midi"],
    "Documents": ["pdf", "doc", "docx", "odt", "rtf", "epub", "mobi", "djvu", "html", "htm", "pages"],
    "Spreadsheets": ["xls", "xlsx", "ods", "numbers"],
    "Presentations": ["ppt", "pptx", "odp", "key"],
    "Text": ["txt", "md", "rst", "log", "tex", "json", "xml", "yaml", "yml", "ini", "conf", "cfg", "toml", "plist", "env"],
    "Data": ["csv", "tsv", "sql", "db", "sqlite", "parquet", "npy", "pkl", "h5"],
    "Code": ["py", "sh", "bash", "zsh", "js", "ts", "c", "h", "cpp", "hpp", "java", "kt", "go", "rs", "rb", "php",
             "cs", "swift", "lua", "pl", "r", "ipynb", "css", "scss", "vue", "jsx", "tsx"],
    "Archives": ["zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "xz", "zst", "lz", "lzma", "cab"],
    "Disk Images": ["iso", "img", "dmg", "qcow2", "vmdk", "vdi", "vhd", "vhdx"],
    "Installers": ["deb", "rpm", "exe", "msi", "apk", "appimage", "snap", "flatpak", "flatpakref", "pkg", "run"],
    "Fonts": ["ttf", "otf", "woff", "woff2"],
    "AI Models": ["gguf", "safetensors", "onnx", "pt", "pth", "ckpt", "tflite", "knm"],
    "Torrents": ["torrent"],
}
RULE_BY_EXTENSION = {extension: folder for folder, extensions in RULE_FOLDERS.items() for extension in extensions}
RULE_ONLY_FOLDERS = {"Spreadsheets", "Presentations", "Disk Images", "Installers", "Fonts", "Torrents"}

RULE_SIGNATURES = [
    (0, b"%PDF", "Documents"), (0, b"\x89PNG", "Images"), (0, b"\xff\xd8\xff", "Images"),
    (0, b"GIF8", "Images"), (0, b"PK\x03\x04", "Archives"), (0, b"Rar!", "Archives"),
    (0, b"7z\xbc\xaf", "Archives"), (0, b"\x1f\x8b", "Archives"), (0, b"BZh", "Archives"),
    (0, b"\xfd7zXZ", "Archives"), (0, b"\x28\xb5\x2f\xfd", "Archives"), (0, b"OggS", "Audio"),
    (0, b"ID3", "Audio"), (0, b"fLaC", "Audio"), (0, b"\x1a\x45\xdf\xa3", "Videos"),
    (0, b"!<arch>\ndebian", "Installers"), (0, b"\xed\xab\xee\xdb", "Installers"), (0, b"MZ", "Installers"),
    (0, b"hsqs", "Installers"), (0, b"\x7fELF", "Programs"), (0, b"GGUF", "AI Models"),
    (0, b"wOFF", "Fonts"), (0, b"wOF2", "Fonts"), (0, b"OTTO", "Fonts"), (0, b"d8:announce", "Torrents"),
    (0, b"SQLite format 3", "Data"), (4, b"ftyp", "Videos"), (32769, b"CD001", "Disk Images"),
]

NAME_SOURCES = [
    (re.compile(r"^(IMG|VID|AUD|PTT|DOC|STK)-\d{8}-WA\d{4}", re.I), "WhatsApp"),
    (re.compile(r"^WhatsApp (Image|Video|Audio|Ptt|Document)", re.I), "WhatsApp"),
    (re.compile(r"^(photo|video|file|audio)_\d+[_@-]", re.I), "Telegram"),
    (re.compile(r"^(Screenshot|Screen Shot)", re.I), "Screenshots"),
    (re.compile(r"^(Screen Recording|Screencast|Screenrecorder|screen-recording)", re.I), "Screen Recordings"),
    (re.compile(r"^(IMG|VID|PXL|DSC|MVIMG)_\d{4,8}", re.I), "Camera"),
]


def extension_of(name):
    lower = name.lower()
    for double in ("tar.gz", "tar.bz2", "tar.xz", "tar.zst"):
        if lower.endswith("." + double):
            return double.split(".")[1]
    return lower.rsplit(".", 1)[1] if "." in lower else ""


def signature_folder(path):
    try:
        with open(path, "rb") as file:
            head = file.read(64)
            file.seek(32769)
            iso = file.read(5)
    except OSError:
        return None
    for offset, magic, folder in RULE_SIGNATURES:
        data = iso if offset == 32769 else head[offset:offset + len(magic)]
        if data.startswith(magic):
            return folder
    if head and all(32 <= b < 127 or b in (9, 10, 13) for b in head):
        return "Text"
    return None


def rule_folder(row):
    folder = RULE_BY_EXTENSION.get(extension_of(row["name"])) or signature_folder(row["path"])
    if folder is None:
        return None
    for pattern, source_folder in NAME_SOURCES:
        if pattern.match(row["name"]):
            if source_folder in ("Screenshots", "Screen Recordings"):
                return source_folder
            if source_folder == "Camera" and folder == "Images":
                return os.path.join(source_folder, "Photos")
            return os.path.join(source_folder, folder)
    return folder


def choose_folder(row, min_confidence):
    rule = rule_folder(row)
    if rule is not None and rule.split(os.sep)[-1] in RULE_ONLY_FOLDERS:
        return rule, "rules"
    if min(row["type_confidence"], row["source_confidence"]) >= min_confidence:
        return folder_for(row["type"], row["source"]), "ai"
    if rule is not None:
        return rule, "rules"
    return RANDOM_FOLDER, "random"


def folder_for(file_type, source):
    top = SOURCE_FOLDERS[source]
    if not top:
        return TYPE_FOLDERS[file_type]
    if source in FLAT_SOURCES:
        return top
    if source == "camera" and file_type == "image":
        return os.path.join(top, "Photos")
    if source == "whatsapp" and file_type == "audio":
        return os.path.join(top, "Voice Notes")
    return os.path.join(top, TYPE_FOLDERS[file_type])


def classify(model, paths):
    rows = []
    vectors = []
    for path in paths:
        with open(path, "rb") as file:
            head = file.read(F.HEAD_BYTES)
        name = os.path.basename(path)
        vectors.append(F.features(name, os.path.getsize(path), head))
        rows.append({"path": path, "name": name})
    if not rows:
        return rows
    type_prob, source_prob = knm.infer(model, np.array(vectors, dtype=np.float32))
    for row, types, sources in zip(rows, type_prob, source_prob):
        row["type"] = F.TYPES[int(types.argmax())]
        row["type_confidence"] = float(types.max())
        row["source"] = F.SOURCES[int(sources.argmax())]
        row["source_confidence"] = float(sources.max())
    return rows


def free_path(path):
    if not os.path.exists(path):
        return path
    stem, extension = os.path.splitext(path)
    number = 1
    while os.path.exists(f"{stem} ({number}){extension}"):
        number += 1
    return f"{stem} ({number}){extension}"


def organize(directory, destination, apply, min_confidence):
    model = knm.load(MODEL)
    paths = sorted(
        os.path.join(directory, name) for name in os.listdir(directory)
        if not name.startswith(".") and os.path.isfile(os.path.join(directory, name))
    )
    rows = classify(model, paths)
    moves = []
    layers = {"ai": 0, "rules": 0, "random": 0}

    print(f"{'file':<46} {'AI type':<16} {'AI source':<20} {'layer':<7} destination")
    for row in rows:
        folder, layer = choose_folder(row, min_confidence)
        layers[layer] += 1
        type_text = f"{row['type']} {row['type_confidence'] * 100:.0f}%"
        source_text = f"{row['source']} {row['source_confidence'] * 100:.0f}%"
        target = free_path(os.path.join(destination, folder, row["name"]))
        moves.append({"from": row["path"], "to": target})
        print(f"{row['name'][:45]:<46} {type_text:<16} {source_text:<20} {layer:<7} {os.path.relpath(target, destination)}")

    print(f"\n{len(rows)} files: {layers['ai']} by the AI, {layers['rules']} by the rules, "
          f"{layers['random']} to {RANDOM_FOLDER}/")

    if not apply:
        print("Nothing moved (plan only). Add --apply to move the files.")
        return

    done = []
    for move in moves:
        os.makedirs(os.path.dirname(move["to"]), exist_ok=True)
        target = free_path(move["to"])
        shutil.move(move["from"], target)
        done.append({"from": move["from"], "to": target})

    log = os.path.join(directory, f"{LOG_PREFIX}{time.strftime('%Y%m%d-%H%M%S')}.json")
    with open(log, "w") as file:
        json.dump({"directory": directory, "destination": destination, "moves": done}, file, indent=1)
    print(f"Moved {len(done)} files. Undo with: python organize.py --undo \"{log}\"")


def undo(log_path):
    with open(log_path) as file:
        log = json.load(file)
    restored = 0
    folders = set()
    for move in reversed(log["moves"]):
        if not os.path.exists(move["to"]):
            print(f"missing, skipped: {move['to']}")
            continue
        if os.path.exists(move["from"]):
            print(f"something else is already at {move['from']}, skipped")
            continue
        os.makedirs(os.path.dirname(move["from"]), exist_ok=True)
        shutil.move(move["to"], move["from"])
        folders.add(os.path.dirname(move["to"]))
        restored += 1
    for folder in sorted(folders, key=len, reverse=True):
        while folder.startswith(log["destination"]) and folder != log["destination"]:
            try:
                os.rmdir(folder)
            except OSError:
                break
            folder = os.path.dirname(folder)
    os.remove(log_path)
    print(f"Restored {restored} files and removed the empty folders")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", nargs="?")
    parser.add_argument("--dest")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--min-confidence", type=float, default=0.9)
    parser.add_argument("--undo")
    args = parser.parse_args()

    if args.undo:
        undo(args.undo)
        return
    if not args.directory or not os.path.isdir(args.directory):
        parser.error("give a directory to organize")
    directory = os.path.abspath(args.directory)
    destination = os.path.abspath(args.dest) if args.dest else directory
    organize(directory, destination, args.apply, args.min_confidence)


if __name__ == "__main__":
    sys.exit(main())
