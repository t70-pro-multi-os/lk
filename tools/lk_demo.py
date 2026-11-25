#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pictures of the boot menu screens as the T70 Pro shows them.

tests/demo_screen runs the real menu code on the build host, drawing into a
2240x1400 frame buffer with the font of your stock lk (G3D1 V1.08), and
prints every shown screen. This script writes them as PNG files:

  countdown.png   the boot wait (any key opens the menu)
  menu.png        the menu
  notice.png      the message after "Boot slot" B and "Continue booting"
  menu-demo.png   all of it as an animation (APNG)

  lk_demo.py out/stock/00-lk.bin out/demo_screen out/demo
"""
import hashlib
import struct
import subprocess
import sys
import zlib
from pathlib import Path

LK_SHA256 = "05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4"


def frames(demo, lk):
    out = subprocess.run([demo, lk], check=True, capture_output=True).stdout
    result, pos = [], 0
    while pos < len(out):
        end = out.index(b"\n", pos)
        tag, ms, w, h = out[pos:end].split()
        if tag != b"F":
            raise SystemExit("bad output from demo_screen")
        w, h, n = int(w), int(h), int(w) * int(h) * 3
        result.append((int(ms), w, h, out[end + 1:end + 1 + n]))
        pos = end + 1 + n
    return result


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def ihdr(w, h):
    return chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))


def pixels(rgb, w, y0=0, y1=None):
    stride = w * 3
    y1 = len(rgb) // stride if y1 is None else y1
    return zlib.compress(b"".join(b"\0" + rgb[y * stride:(y + 1) * stride] for y in range(y0, y1)), 9)


def write_png(path, w, h, rgb):
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + ihdr(w, h) + chunk(b"IDAT", pixels(rgb, w)) + chunk(b"IEND", b""))


def write_apng(path, w, h, seq):
    """seq: (delay_ms, rgb) per frame; later frames only carry the rows that changed."""
    stride = w * 3
    out = [b"\x89PNG\r\n\x1a\n", ihdr(w, h), chunk(b"acTL", struct.pack(">II", len(seq), 0))]
    n, prev = 0, None
    for delay, rgb in seq:
        y0, y1 = 0, h
        if prev is not None:
            changed = [y for y in range(h) if rgb[y * stride:(y + 1) * stride] != prev[y * stride:(y + 1) * stride]]
            y0, y1 = (changed[0], changed[-1] + 1) if changed else (0, 1)
        out.append(chunk(b"fcTL", struct.pack(">IIIIIHHBB", n, w, y1 - y0, 0, y0, delay, 1000, 0, 0)))
        n += 1
        data = pixels(rgb, w, y0, y1)
        if prev is None:
            out.append(chunk(b"IDAT", data))
        else:
            out.append(chunk(b"fdAT", struct.pack(">I", n) + data))
            n += 1
        prev = rgb
    out.append(chunk(b"IEND", b""))
    path.write_bytes(b"".join(out))


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    lk, demo, out = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    if hashlib.sha256(Path(lk).read_bytes()).hexdigest() != LK_SHA256:
        raise SystemExit("not the G3D1 V1.08 lk payload")
    out.mkdir(parents=True, exist_ok=True)
    seq = frames(demo, lk)
    w, h = seq[0][1], seq[0][2]
    # countdown: before the first key (1.5 s); menu: as it opened; notice: the last screen
    stills = {"countdown.png": next(f for f in seq if f[0] >= 600),
              "menu.png": next(f for f in seq if f[0] >= 1500),
              "notice.png": seq[-1]}
    for name, (_, _, _, rgb) in stills.items():
        write_png(out / name, w, h, rgb)
    black = bytes(w * h * 3)
    anim = [(max(seq[i + 1][0] - t, 100) if i + 1 < len(seq) else 2000, rgb)
            for i, (t, _, _, rgb) in enumerate(seq)] + [(1000, black)]
    write_apng(out / "menu-demo.png", w, h, anim)
    for name in list(stills) + ["menu-demo.png"]:
        print(f"{out / name}: {(out / name).stat().st_size} bytes")


if __name__ == "__main__":
    main()
