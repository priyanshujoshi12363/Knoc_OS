#!/usr/bin/env python3
import os
import random
import struct
import sys

rng = random.Random(7)


def filler(size):
    return rng.randbytes(size)


def write(directory, name, data):
    with open(os.path.join(directory, name), "wb") as file:
        file.write(data)


def main():
    directory = sys.argv[1]
    os.makedirs(directory, exist_ok=True)

    pdf = (b"%PDF-1.3\n%\xe2\xe3\xcf\xd3\n31 0 obj\n<<\n/Type /ExtGState\n/ca 0.3\n/CA 0.3\n>>\nendobj\n"
           b"6 0 obj\n<<\n/Type /Page\n/Parent 1 0 R\n/MediaBox [0 0 595 842]\n>>\nendobj\n")
    write(directory, "335505283.pdf", pdf + filler(3000))

    jpeg = b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00\xff\xdb\x00\x43\x00"
    write(directory, "WhatsApp Image 2025-12-13 at 2.46.41 PM.jpeg", jpeg + filler(6000))

    png = (b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR" + struct.pack(">II", 1920, 1080)
           + bytes([8, 6, 0, 0, 0]) + filler(4))
    write(directory, "Screenshot from 2026-06-10 08-06-32.png", png + filler(5000))

    zip_header = b"PK\x03\x04\x14\x00\x00\x00\x08\x00" + filler(16) + struct.pack("<HH", 13, 0) + b"notes/day.txt"
    write(directory, "files.zip", zip_header + filler(2000))
    write(directory, "Cloudpath-x64.tar.bz2", b"BZh91AY&SY" + filler(4000))

    write(directory, "install.sh", b"#!/usr/bin/env bash\nset -e\n\nsudo apt-get update\nsudo apt-get install -y git\n"
                                   b"echo \"done\"\n")
    write(directory, "google-services.json", b'{\n  "project_info": {\n    "project_number": "4821",\n'
                                             b'    "project_id": "demo-app"\n  },\n  "client": []\n}\n')
    write(directory, "notes.txt", b"shopping list\n- milk\n- bread\n\ncall the bank on monday\n")
    write(directory, "gbn3n9qdtiaodj6boc7y", b"a8f3kd92lsm4xq7r1zp0c5vb6nwe8ty2ui9o")

    elf = (b"\x7fELF\x02\x01\x01\x00" + bytes(8) + struct.pack("<HHI", 3, 243, 1)
           + struct.pack("<QQQ", 0x10000, 64, 0) + filler(40))
    write(directory, "backup-tool", elf + filler(6000))

    iso = bytearray(40000)
    iso[32769:32774] = b"CD001"
    write(directory, "ubuntu-24.04-desktop-amd64.iso", bytes(iso))
    write(directory, "code_1.93.1_amd64.deb", b"!<arch>\ndebian-binary   " + filler(1000))
    office = b"PK\x03\x04\x14\x00\x00\x00\x08\x00" + filler(16) + struct.pack("<HH", 19, 0) + b"[Content_Types].xml"
    write(directory, "budget_2024.xlsx", office + filler(1500))
    write(directory, "mystery.xyz", filler(700))


if __name__ == "__main__":
    main()
