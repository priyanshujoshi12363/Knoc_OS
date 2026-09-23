import os
import subprocess
import sys

import numpy as np

import features as F
import knm
import organize

MIME_TYPES = [
    ("image/", "image"), ("video/", "video"), ("audio/", "audio"),
    ("application/pdf", "document"), ("text/html", "document"), ("application/rtf", "document"),
    ("application/epub", "document"), ("application/vnd.oasis.opendocument.text", "document"),
    ("application/msword", "document"),
    ("application/vnd.openxmlformats-officedocument.wordprocessingml", "document"),
    ("text/x-script.python", "code"), ("text/x-python", "code"), ("text/x-shellscript", "code"),
    ("text/x-c", "code"), ("text/x-java", "code"), ("application/javascript", "code"),
    ("text/javascript", "code"),
    ("application/json", "text"), ("text/xml", "text"), ("application/xml", "text"),
    ("text/csv", "text"), ("text/", "text"),
    ("application/x-executable", "program"), ("application/x-pie-executable", "program"),
    ("application/x-sharedlib", "program"),
    ("application/zip", "archive"), ("application/gzip", "archive"), ("application/x-bzip2", "archive"),
    ("application/x-xz", "archive"), ("application/x-tar", "archive"), ("application/x-7z", "archive"),
    ("application/vnd.rar", "archive"), ("application/x-rar", "archive"), ("application/zstd", "archive"),
    ("font/", "data"), ("application/vnd.sqlite3", "data"), ("application/x-sqlite3", "data"),
]


def truth(path):
    mime = subprocess.run(["file", "-b", "--mime-type", path], capture_output=True, text=True).stdout.strip()
    for prefix, label in MIME_TYPES:
        if mime.startswith(prefix):
            return label, mime
    return None, mime


def main():
    directories = sys.argv[1:] or [os.path.expanduser("~/Downloads")]
    model = knm.load(organize.MODEL)
    checked = correct = sure = sure_correct = 0
    for directory in directories:
        paths = sorted(os.path.join(directory, n) for n in os.listdir(directory)
                       if not n.startswith(".") and os.path.isfile(os.path.join(directory, n)))
        for row in organize.classify(model, paths):
            expected, mime = truth(row["path"])
            if expected is None:
                continue
            checked += 1
            hit = row["type"] == expected
            correct += hit
            if row["type_confidence"] >= 0.9:
                sure += 1
                sure_correct += hit
            if not hit:
                print(f"  {row['name'][:44]:<44} model {row['type']} {row['type_confidence'] * 100:.0f}%"
                      f"   file says {expected} ({mime})")
    if checked:
        print(f"\nreal files checked: {checked}")
        print(f"type correct: {correct}/{checked} = {correct / checked * 100:.1f}%")
        print(f"when sure (>= 90%): {sure_correct}/{sure} correct, {checked - sure} left alone")


if __name__ == "__main__":
    main()
