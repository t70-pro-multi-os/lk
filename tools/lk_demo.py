#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draw the boot menu screens as the T70 Pro shows them.

tests/demo_screen runs the real menu code on the build host and prints each
screen update as text (87 rows of 280 columns, the stock console on the
2240x1400 panel). This script draws those frames with the 8x16 font and the
colours of your stock lk (G3D1 V1.08) and writes:

  screen.png      the whole panel with the menu open (real size)
  boot-menu.png   the menu, the text area enlarged 2x
  boot-menu.gif   the sequence: countdown, menu, move, select, reboot

  lk_demo.py out/stock/00-lk.bin out/demo_screen out/demo
"""
import hashlib
import struct
import subprocess
import sys
import zlib
from pathlib import Path

LK_SHA256 = "05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4"
FONT = 0xB1100            # 256 glyphs x 16 rows, 1 bit per pixel, MSB left (used by f_042e38)
ROWS, COLS = 87, 280
PANEL_H = 1400            # 87 rows of 16 pixels leave 8 pixel rows at the bottom
FG, BG = (0xA0, 0xA0, 0xA0), (0, 0, 0)  # console colours set by f_04381c
CROP = (35, 51, 0, 56)    # rows and columns around the menu (centred at row 43)
SCALE = 2


def frames(demo):
    out = subprocess.run([demo], check=True, capture_output=True, text=True).stdout
    lines = out.split("\n")
    result = []
    for i, line in enumerate(lines):
        if line.startswith("F "):
            screen = tuple(row.ljust(COLS) for row in lines[i + 1:i + 1 + ROWS])
            if len(screen) != ROWS:
                raise SystemExit("short frame from demo_screen")
            if not result or result[-1][1] != screen:
                result.append((int(line[2:]), screen))
    return result


def bit_rows(font, screen, crop, scale):
    """1-bit pixel rows (MSB = left pixel) for the text in crop, enlarged by scale."""
    r0, r1, c0, c1 = crop
    widen = [bytes([b]) for b in range(256)]
    if scale == 2:
        widen = []
        for b in range(256):
            v = 0
            for bit in range(8):
                if b & (0x80 >> bit):
                    v |= 0xC000 >> (2 * bit)
            widen.append(v.to_bytes(2, "big"))
    rows = []
    for r in range(r0, r1):
        text = screen[r][c0:c1].encode("ascii", "replace")
        for y in range(16):
            row = b"".join(widen[font[ch * 16 + y]] for ch in text)
            rows.extend([row] * scale)
    return rows


def write_png(path, width, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    raw = b"".join(b"\0" + r for r in rows)
    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", width, len(rows), 1, 3, 0, 0, 0))
                     + chunk(b"PLTE", bytes(BG + FG))
                     + chunk(b"IDAT", zlib.compress(raw, 9))
                     + chunk(b"IEND", b""))


def lzw(pixels, min_size=2):
    """GIF LZW (variable code size, LSB first)."""
    clear, eoi = 1 << min_size, (1 << min_size) + 1
    size, nxt, table = min_size + 1, eoi + 1, {}
    out, acc, nacc = bytearray(), 0, 0

    def emit(code):
        nonlocal acc, nacc
        acc |= code << nacc
        nacc += size
        while nacc >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nacc -= 8

    emit(clear)
    prefix = pixels[0]
    for k in pixels[1:]:
        key = (prefix << 8) | k
        if key in table:
            prefix = table[key]
            continue
        emit(prefix)
        if nxt < 4096:
            table[key] = nxt
            if nxt == 1 << size and size < 12:
                size += 1
            nxt += 1
        else:
            emit(clear)
            table, size, nxt = {}, min_size + 1, eoi + 1
        prefix = k
    emit(prefix)
    emit(eoi)
    if nacc:
        out.append(acc & 0xFF)
    return bytes(out)


def write_gif(path, width, height, images):
    """images: (delay_ms, x, y, w, h, 1-bit rows) per frame, rows cover the sub-rectangle."""
    spread = [bytes((b >> (7 - i)) & 1 for i in range(8)) for b in range(256)]
    gif = bytearray(b"GIF89a" + struct.pack("<HHBBB", width, height, 0x80, 0, 0) + bytes(BG + FG))
    gif += b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00"          # loop forever
    for delay, x, y, w, h, rows in images:
        pixels = b"".join(b"".join(spread[b] for b in row)[:w] for row in rows)
        gif += b"\x21\xf9\x04\x00" + struct.pack("<H", max(2, delay // 10)) + b"\x00\x00"
        gif += b"\x2c" + struct.pack("<HHHHB", x, y, w, h, 0) + b"\x02"
        data = lzw(pixels)
        for i in range(0, len(data), 255):
            gif += bytes([len(data[i:i + 255])]) + data[i:i + 255]
        gif += b"\x00"
    gif += b"\x3b"
    path.write_bytes(bytes(gif))


def changed_cells(prev, screen, crop):
    """Bounding box (in cells, inside crop) of what changed since prev."""
    r0, r1, c0, c1 = crop
    rows = [r for r in range(r0, r1) if prev[r][c0:c1] != screen[r][c0:c1]]
    if not rows:
        return None
    cols = [c for r in rows for c in range(c0, c1) if prev[r][c] != screen[r][c]]
    return rows[0], rows[-1] + 1, min(cols), max(cols) + 1


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    lk, demo, out = Path(sys.argv[1]).read_bytes(), sys.argv[2], Path(sys.argv[3])
    if hashlib.sha256(lk).hexdigest() != LK_SHA256:
        raise SystemExit("not the G3D1 V1.08 lk payload")
    font = lk[FONT:FONT + 256 * 16]
    out.mkdir(parents=True, exist_ok=True)

    seq = frames(demo)
    menu = next(s for t, s in seq if any("T70 boot menu" in row for row in s))
    rows = bit_rows(font, menu, (0, ROWS, 0, COLS), 1)
    write_png(out / "screen.png", COLS * 8, rows + [bytes(COLS)] * (PANEL_H - len(rows)))
    r0, r1, c0, c1 = CROP
    cw, ch = (c1 - c0) * 8 * SCALE, (r1 - r0) * 16 * SCALE
    write_png(out / "boot-menu.png", cw, bit_rows(font, menu, CROP, SCALE))

    # The animation ends with the screen going dark for the reboot.
    blank = tuple(" " * COLS for _ in range(ROWS))
    seq.append((seq[-1][0] + 2500, blank))
    images, prev = [], None
    for i, (t, screen) in enumerate(seq):
        delay = seq[i + 1][0] - t if i + 1 < len(seq) else 1200
        delay = max(delay, 900)
        box = CROP if prev is None else changed_cells(prev, screen, CROP)
        prev = screen
        if box is None:
            continue
        br0, br1, bc0, bc1 = box
        rows = bit_rows(font, screen, box, SCALE)
        images.append((delay, (bc0 - c0) * 8 * SCALE, (br0 - r0) * 16 * SCALE,
                       (bc1 - bc0) * 8 * SCALE, (br1 - br0) * 16 * SCALE, rows))
    write_gif(out / "boot-menu.gif", cw, ch, images)
    for name in ("screen.png", "boot-menu.png", "boot-menu.gif"):
        print(f"{out / name}: {(out / name).stat().st_size} bytes")


if __name__ == "__main__":
    main()
