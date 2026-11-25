#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare a built lk payload with the stock one; print differing 4-byte words.

    lk_compare.py STOCK BUILT [--fallback-out FILE]
"""
import hashlib
import struct
import sys
from pathlib import Path

args = sys.argv[1:]
out = None
if "--fallback-out" in args:
    i = args.index("--fallback-out")
    out = Path(args[i + 1])
    del args[i:i + 2]
a, b = (Path(x).read_bytes() for x in args[:2])
print(f"stock {len(a):#x} {hashlib.sha256(a).hexdigest()}")
print(f"built {len(b):#x} {hashlib.sha256(b).hexdigest()}")
diff = [o for o in range(0, min(len(a), len(b)) - 3, 4) if a[o:o + 4] != b[o:o + 4]]
for o in diff[:40]:
    print(f"  {o:#08x}: stock {struct.unpack_from('<I', a, o)[0]:08x} built {struct.unpack_from('<I', b, o)[0]:08x}")
if out is not None:
    out.write_text("".join(f"{o:x}\n" for o in diff))
if len(a) != len(b) or diff:
    sys.exit(f"MISMATCH: {len(diff)} words differ, sizes {len(a):#x}/{len(b):#x}")
print("IDENTICAL")
