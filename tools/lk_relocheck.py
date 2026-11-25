#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that a built lk keeps every stock reference intact.

    lk_relocheck.py STOCK_LK_BIN GEN_DIR LK_ELF [--names F] [--hooks F]
        Check a build made from GEN_DIR (generated with the same names/hooks).
    lk_relocheck.py STOCK_LK_BIN --pad-test WORK [OFFSET=BYTES ...]
        Generate the source, insert NOP padding after the given chunks, build
        with STOCK_LAYOUT=0 and check that build.

For every stock instruction and data byte the check finds where it went in the
build (chunk start symbols, section moves and hook size changes) and compares:
branch and literal targets, adrp page + :lo12: sums, adr_far pairs, absolute
pointers, switch jump-table entries and vector-table alignment must point to
the moved targets; every other word and byte must be unchanged. Words replaced
by hooks are skipped. It also checks that no add/ldr/str whose base register
was last set by an adrp was left without a :lo12: symbol.

This is a static check of the built image, not a boot test.
"""
import bisect
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import lk_disasm as L  # noqa: E402

ROOT = HERE.parent


def read_list(path, kind):
    out = {}
    for raw in Path(path).read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if kind == "names":
            off, name = line.split()
            out[int(off, 16)] = name
        else:
            head, body = line.split(":", 1)
            off, count = head.split()
            out[int(off, 16)] = (int(count), [x.strip() for x in body.split(";") if x.strip()])
    return out


def check(data, lk, elf, pads=None):
    built = elf.with_suffix(".bin").read_bytes()
    nm = subprocess.run([f"{L.CROSS}nm", str(elf)], check=True, capture_output=True, text=True).stdout
    sym = {}
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 3:
            sym[parts[2]] = int(parts[0], 16) - L.BASE
    starts, hooks = lk.starts, lk.hooks
    hooked = set()
    for h, (count, _) in hooks.items():
        hooked.update(range(h, h + 4 * count, 4))

    deltas = {}
    for t, name in lk.names.items():
        if name.startswith((".L", "f_", "L_")) or name not in sym:
            continue
        sec = "bss" if t >= L.FILE_END else L.section_of(t)
        if sec != "text":
            deltas.setdefault(sec, set()).add(sym[name] - t)
    fails = []
    for sec, ds in deltas.items():
        if len(ds) != 1:
            fails.append(f"section {sec} moved unevenly: {sorted(ds)[:4]}")
    delta = {k: min(v) for k, v in deltas.items()}

    def mp(t):
        if t < lk.text_end:
            c = starts[bisect.bisect_right(starts, t) - 1]
            shift = sum((len(ins) - n) * 4 for h, (n, ins) in hooks.items()
                        if c <= h and h + 4 * n <= t and starts[bisect.bisect_right(starts, h) - 1] == c)
            return sym[f"f_{c:06x}"] + (t - c) + shift
        if t == lk.text_end:
            return sym[lk.names[t]]
        return t + delta["bss" if t >= L.FILE_END else L.section_of(t)]

    def bw(o):
        return struct.unpack_from("<I", built, o)[0]

    checked = {"same": 0, "ref": 0, "adrp_pairs": 0, "pointer": 0, "jt_entries": 0}
    for t in lk.vbar_targets:
        if mp(t) % 0x800:
            fails.append(f"vector table {t:#x} lands at {mp(t):#x}")
    for o in range(0, lk.text_end, 4):
        if o in hooked:
            continue
        o2 = mp(o)
        wa, wb = lk.word(o), bw(o2)
        if o in lk.lit_words:
            base = o - 4 if (o - 4) in lk.pointers else o
            if base in lk.pointers:
                if o == base:
                    checked["pointer"] += 1
                    if struct.unpack_from("<Q", built, o2)[0] != L.BASE + mp(lk.pointers[o]):
                        fails.append(f"literal pointer {o:#x}")
                continue
            checked["same"] += 1
            if wa != wb:
                fails.append(f"literal {o:#x} changed")
            continue
        if o + 4 in lk.far_adr and o + 4 not in hooked:
            continue                                  # checked with its adr
        if o in lk.far_adr:
            r2, user = L.decode_ref(bw(mp(o - 4)), mp(o - 4)), L.lo12_user(wb)
            checked["ref"] += 1
            if not r2 or r2[0] != "adrp" or not user or user[0] != "add" or r2[1] + user[2] != mp(lk.far_adr[o]):
                fails.append(f"far adr {o:#x}")
            continue
        ref = lk.refs.get(o)
        if ref and ref[0] != "adrp":
            r2 = L.decode_ref(wb, o2)
            checked["ref"] += 1
            if r2 is None or r2[0] != ref[0] or r2[1] != mp(ref[1]):
                fails.append(f"ref {o:#x} {ref[0]} -> {ref[1]:#x}: built {r2}")
            continue
        if o in lk.adrp_sym:
            page = L.decode_ref(wb, o2)[1]
            for u in lk.adrp_users[o]:
                if u in hooked:
                    continue
                user = L.lo12_user(bw(mp(u)))
                checked["adrp_pairs"] += 1
                if not user or page + user[2] != mp(lk.lo12[u][1]):
                    fails.append(f"adrp {o:#x} + user {u:#x}")
            continue
        if o in lk.lo12:
            if (wa & ~0x003ffc00) != (wb & ~0x003ffc00):
                fails.append(f"lo12 user {o:#x} changed outside the immediate")
            continue
        checked["same"] += 1
        if wa != wb:
            fails.append(f"word {o:#x} changed {wa:08x} -> {wb:08x}")
    for start, end in ((L.RODATA, lk.rodata_end), (L.DATA, L.FILE_END)):
        o = start
        while o < end:
            if o in lk.pointers:
                checked["pointer"] += 1
                if struct.unpack_from("<Q", built, mp(o))[0] != L.BASE + mp(lk.pointers[o]):
                    fails.append(f"pointer {o:#x}")
                o += 8
                continue
            if data[o] != built[mp(o)]:
                fails.append(f"data byte {o:#x} changed")
            o += 1
    for jt in lk.jump_tables.values():
        fmt = {1: "b", 2: "h", 4: "i"}[jt["size"]]
        want = {mp(t) for t in jt["targets"]}
        for i in range(jt["entries"]):
            at = mp(jt["table"]) + i * jt["size"]
            raw = built[at:at + jt["size"]]
            vals = struct.unpack("<" + fmt, raw)[0], struct.unpack("<" + fmt.upper(), raw)[0]
            checked["jt_entries"] += 1
            if not any(mp(jt["anchor"]) + (v << jt["shift"]) in want for v in vals):
                fails.append(f"jump table {jt['br']:#x} entry {i}")

    adrps = {o for o, r in lk.refs.items() if r[0] == "adrp"}
    missed = [o for o in lk.ins if o not in lk.lo12 and o not in lk.lit_words and L.lo12_user(lk.word(o))
              and (d := lk.last_def(o, L.lo12_user(lk.word(o))[1], 64, fallthrough_only=True)) is not None
              and d in adrps]
    if missed:
        fails.append(f"{len(missed)} lo12-capable instructions after an adrp are not symbolised: "
                     + " ".join(f"{m:#x}" for m in missed[:10]))
    desc = f"pads {', '.join(f'{k:#x}+{v:#x}' for k, v in pads.items())}; " if pads else ""
    desc += f"hooks {len(hooks)}; " if hooks else ""
    print(f"{desc}section moves {', '.join(f'{k} {v:+#x}' for k, v in sorted(delta.items()))}; "
          f"lk {len(built):#x} bytes")
    print("checked", checked)
    for f in fails[:40]:
        print("FAIL", f)
    if fails:
        sys.exit(f"{len(fails)} failures")
    print("RELOCATABLE: every stock reference follows the moved code and data")


def main(argv):
    args = list(argv)
    opts = {}
    for opt in ("--names", "--hooks"):
        if opt in args:
            i = args.index(opt)
            opts[opt] = read_list(args[i + 1], opt[2:])
            del args[i:i + 2]
    data = Path(args[0]).read_bytes()
    lk = L.Lk(data, names=opts.get("--names"), hooks=opts.get("--hooks"))
    lk.analyse()
    lk.plan()
    if args[1] == "--pad-test":
        work = Path(args[2])
        pads = dict((int(k, 16), int(v, 0)) for k, v in (a.split("=") for a in args[3:])) or {
            0x0001a8: 0x104, 0x03eaf0: 0x1234}
        if work.exists():
            shutil.rmtree(work)
        lk.write(work / "gen")
        for after, size in pads.items():
            assert lk.chunk_key(after) == after, f"{after:#x} is not a chunk start"
            (work / "gen/text" / f"{after:06x}_pad.S").write_text(
                f'\t.section .text.lk.{after:06x}_pad, "ax", %progbits\n\t.p2align 2\n'
                f"\t.fill {size // 4}, 4, 0xd503201f\n")
        subprocess.run(["make", "-s", "-C", str(ROOT), "-f", "build.mk", "-j8", f"SRC={work / 'gen'}",
                        f"OUT={work / 'build'}", "STOCK_LAYOUT=0"], check=True)
        check(data, lk, work / "build/lk.elf", pads)
    else:
        check(data, lk, Path(args[2]))


if __name__ == "__main__":
    main(sys.argv[1:])
