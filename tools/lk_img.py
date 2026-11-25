#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Split and repack a MediaTek "LK v2" partition image (lk.img).

The image is a chain of parts. Each part has a 512-byte header
(magic 0x58881688, payload size, 32-byte name, load address, mode, extended
header) followed by the payload, padded to 16 bytes. The T70 Pro G3D1 V1.08
image holds lk, bl2_ext, aee, lk_main_dtb and lk_dtbo, each followed by its
cert1 and cert2 parts.

    lk_img.py list   lk.img
    lk_img.py split  lk.img OUTDIR          # writes <index>-<name>.bin + parts.json
    lk_img.py repack lk.img OUT.img name=FILE [name=FILE ...]

repack keeps every part of the input image (headers, certificates and the
other stages) and replaces only the named payloads, fixing the size field.
With the device's secure boot off and the bootloader unlocked, the stock
preloader and bl2_ext accept a changed lk payload (seen on the device on
2026-11-24 with a patched lk); the certificates are not re-signed.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

MAGIC = 0x58881688
HEADER = 512


def parts(data):
    off = 0
    while off + HEADER <= len(data):
        magic, size = struct.unpack_from("<II", data, off)
        if magic != MAGIC:
            break
        name = data[off + 8:off + 40].split(b"\0")[0].decode()
        end = off + HEADER + size
        if end > len(data):
            raise ValueError(f"part {name} at {off:#x} runs past the image")
        yield {"offset": off, "name": name, "size": size,
               "header": bytes(data[off:off + HEADER]),
               "payload": bytes(data[off + HEADER:end])}
        off = (end + 15) & ~15
    if any(data[off:]):
        raise ValueError(f"non-zero bytes after the last part at {off:#x}")


PARTITION = 4 * 1024 * 1024    # lk_a / lk_b on the T70 Pro


def build(items, total=None):
    """Chain the parts; pad to total if the result is smaller (the stock size)."""
    out = bytearray()
    for item in items:
        header = bytearray(item["header"])
        struct.pack_into("<I", header, 4, len(item["payload"]))
        out += header + item["payload"]
        out += b"\0" * (-len(out) % 16)
    if len(out) > PARTITION:
        raise ValueError(f"image of {len(out):#x} bytes does not fit the {PARTITION:#x}-byte lk partition")
    if total is not None and len(out) < total:
        out += b"\0" * (total - len(out))
    return bytes(out)


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    cmd, image = argv[0], Path(argv[1])
    data = image.read_bytes()
    items = list(parts(data))
    if cmd == "list":
        for i, p in enumerate(items):
            print(f"{i:2} {p['offset']:#09x} {p['name']:12} {p['size']:#9x} "
                  f"{hashlib.sha256(p['payload']).hexdigest()}")
    elif cmd == "split":
        out = Path(argv[2])
        out.mkdir(parents=True, exist_ok=True)
        record = []
        for i, p in enumerate(items):
            (out / f"{i:02}-{p['name']}.bin").write_bytes(p["payload"])
            record.append({"index": i, "name": p["name"], "offset": p["offset"], "size": p["size"],
                           "sha256": hashlib.sha256(p["payload"]).hexdigest()})
        (out / "parts.json").write_text(json.dumps(
            {"image": image.name, "image_size": len(data),
             "image_sha256": hashlib.sha256(data).hexdigest(), "parts": record}, indent=2) + "\n")
    elif cmd == "repack":
        output = Path(argv[2])
        repl = dict(a.split("=", 1) for a in argv[3:])
        names = [p["name"] for p in items]
        for name in repl:
            if names.count(name) != 1:
                sys.exit(f"part {name!r} is not exactly once in {image}")
        for p in items:
            if p["name"] in repl:
                p["payload"] = Path(repl[p["name"]]).read_bytes()
        built = build(items, total=len(data))
        output.write_bytes(built)
        print(f"{output}: {len(built):#x} bytes sha256 {hashlib.sha256(built).hexdigest()}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
