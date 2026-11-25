#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Print the record the diagnostic build (make DIAG=1) leaves in misc.

  adb shell dd if=/dev/block/by-name/misc bs=512 skip=25 count=1 > diag.bin
  lk_diag.py diag.bin

Layout: struct t70_diag in src/t70.h, at misc offset 0x3200.
"""
import struct
import sys

MAGIC = 0x44303754  # "T70D"
FORMAT = "<4I4Q6IQ2Ii4I4I4I"
FIELDS = ("magic seq size_of stage t_entry t_last fbcon fb size colours bytes_pp width height "
          "disp_ready disp_fb disp_off boot_mode gfx wait_ms keys_start flushes misc_write").split()
STAGES = {1: "started (console lines shown)", 2: "no usable frame buffer: waited only",
          3: "wait off, no key: nothing drawn", 4: "countdown drawn", 5: "menu opened",
          6: "wait over, booting", 7: "menu closed, booting", 8: "boot mode already chosen: no wait"}


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    data = open(sys.argv[1], "rb").read()
    values = struct.unpack_from(FORMAT, data)
    rec = dict(zip(FIELDS, values[:len(FIELDS)]))
    px = values[len(FIELDS):len(FIELDS) + 4]
    samples, kpd_and, kpd_or, det_or = values[len(FIELDS) + 4:]
    if rec["magic"] != MAGIC:
        raise SystemExit("no diagnostic record (the diagnostic lk has not run)")
    if rec["size_of"] != 144:
        raise SystemExit(f"record size {rec['size_of']}, expected 144: tool and lk differ")
    print(f"boot #{rec['seq']}: {STAGES.get(rec['stage'], rec['stage'])}")
    print(f"  times: first record {rec['t_entry'] / 1e6:.3f} s, this one {rec['t_last'] / 1e6:.3f} s after lk start")
    print(f"  lk_fbcon {rec['fbcon']:#x}: fb {rec['fb']:#x}, {rec['width']}x{rec['height']}, "
          f"{rec['bytes_pp']} bytes per pixel, size {rec['size']:#x}, colours {rec['colours']}")
    print(f"  display: ready {rec['disp_ready']}, frame buffer {rec['disp_fb']:#x} + console page {rec['disp_off']:#x}")
    print(f"  gfx_init {rec['gfx']}, wait {rec['wait_ms']} ms, keys at start {rec['keys_start']:#x}, "
          f"boot mode {rec['boot_mode']}, flushes {rec['flushes']}, previous misc write {rec['misc_write']}")
    print(f"  pixels: console page first {px[0]:#010x} (0 expected), bar {px[1]:#010x} (0xff8c8c8c expected); "
          f"page 0 first {px[2]:#010x}, bar {px[3]:#010x}")
    seen = [name for bit, name in enumerate(("vol-down", "vol-up", "power", "home")) if det_or >> bit & 1]
    print(f"  keys: {samples} keypad reads, AND {kpd_and:#06x} OR {kpd_or:#06x}; "
          f"lk_detect_key seen pressed: {', '.join(seen) or 'none'}")


if __name__ == "__main__":
    main()
