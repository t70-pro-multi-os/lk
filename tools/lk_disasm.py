#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn the stock T70 Pro LK stage (BL33 "lk" part) into re-assemblable source.

    lk_disasm.py LK_BIN OUTDIR [--fallback FILE] [--names FILE] [--hooks FILE]

Every PC-relative reference (b, bl, b.cond, cbz, tbz, adr, ldr literal), every
adrp with its :lo12: users and every absolute pointer in data becomes a symbol,
so the code can be moved and grown. Everything else is written as the
disassembled instruction or as data directives. Instructions whose text the GNU
assembler encodes differently are listed in --fallback and written as .inst.

The output (OUTDIR/text/*.S, rodata.S, data.S, bss.S, lk.ld, layout.json)
links back to the input byte for byte at the stock address.

--names: lines "OFFSET NAME"; NAME becomes a linker alias of the stock label at
OFFSET (a label is made there if the stock code has none).
--hooks: lines "OFFSET COUNT: INSN; INSN; ..." replace COUNT stock instruction
words at OFFSET with the given instructions (any number). Hooks change the
layout, so they are only for builds with LK_STOCK_LAYOUT=0.
"""
import hashlib
import json
import os
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs

BASE = 0xffff000050f00000
CROSS = os.environ.get("CROSS", "aarch64-linux-gnu-")
STOCK_SHA = "05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4"
# Section starts, from the stock start.S literal pool (0x2f8..0x368) and the
# page layout: rodata 0x91000, data 0xb9000, file end 0xc4c00, end of bss 0x185000.
TEXT, RODATA, DATA, FILE_END, BSS_END = 0, 0x91000, 0xb9000, 0xc4c00, 0x185000
# (adrp, read) pairs checked by hand: the page register is only saved by the prologue
# of the next function, reached by falling through after a call to a noreturn function.
NOP = 0xd503201f
REVIEWED_ESCAPES = {(0x17308, 0x17464), (0x1c4d4, 0x1c590), (0x1c4e0, 0x1c590)}


def sx(value, bits):
    return value - (1 << bits) if value >> (bits - 1) & 1 else value


def section_of(off):
    if off < RODATA:
        return "text"
    if off < DATA:
        return "rodata"
    if off < FILE_END:
        return "data"
    return "bss"


def decode_ref(w, off):
    """PC-relative reference: (kind, target offset[, size])."""
    if (w & 0x7c000000) == 0x14000000:
        return ("bl" if w >> 31 else "b", off + sx(w & 0x3ffffff, 26) * 4)
    if (w & 0xff000010) == 0x54000000:
        return ("bcond", off + sx((w >> 5) & 0x7ffff, 19) * 4)
    if (w & 0x7e000000) == 0x34000000:
        return ("cbz", off + sx((w >> 5) & 0x7ffff, 19) * 4)
    if (w & 0x7e000000) == 0x36000000:
        return ("tbz", off + sx((w >> 5) & 0x3fff, 14) * 4)
    if (w & 0x3b000000) == 0x18000000:
        opc, v = w >> 30, (w >> 26) & 1
        size = {(0, 0): 4, (1, 0): 8, (2, 0): 4, (3, 0): 0, (0, 1): 4, (1, 1): 8, (2, 1): 16}.get((opc, v))
        if size is None:
            return None
        return ("ldrlit", off + sx((w >> 5) & 0x7ffff, 19) * 4, size)
    if (w & 0x1f000000) == 0x10000000:
        imm = sx((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21)
        if w >> 31:
            return ("adrp", (off & ~0xfff) + (imm << 12))
        return ("adr", off + imm)
    return None


def lo12_user(w):
    """(kind, base register, byte offset) for add-immediate and unsigned-offset loads/stores."""
    if (w & 0x7fc00000) == 0x11000000:          # add (immediate), no shift
        return ("add", (w >> 5) & 31, (w >> 10) & 0xfff)
    if (w & 0x3b000000) == 0x39000000:          # ldr/str/prfm (unsigned offset)
        size, v, opc = w >> 30, (w >> 26) & 1, (w >> 22) & 3
        scale = 4 if (v and size == 0 and opc & 2) else size
        return ("ldst", (w >> 5) & 31, ((w >> 10) & 0xfff) << scale)
    return None


def is_ret_or_br(w):
    return (w & 0xfffffc1f) in (0xd65f0000, 0xd61f0000) or w == 0xd69f03e0


def is_call(w):
    return (w & 0xfc000000) == 0x94000000 or (w & 0xfffffc1f) == 0xd63f0000


REG = re.compile(r"^[wx](\d+)$")


class Lk:
    def __init__(self, data, fallback=(), names=None, hooks=None):
        self.d = data
        self.user_names = names or {}     # offset -> name
        self.hooks = hooks or {}          # offset -> (count, [insn, ...])
        self.md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        self.md.detail = True
        self.fallback = set(fallback)
        self.ins = {}            # text offset -> (mnemonic, op_str, reads, writes)
        self.notes = defaultdict(list)

    def word(self, off):
        return struct.unpack_from("<I", self.d, off)[0]

    def quad(self, off):
        return struct.unpack_from("<Q", self.d, off)[0]

    def regnum(self, r):
        m = REG.match(self.md.reg_name(r) or "")
        return int(m.group(1)) if m else None

    def disassemble(self):
        for off in range(TEXT, RODATA, 4):
            got = list(self.md.disasm(self.d[off:off + 4], BASE + off, count=1))
            if not got:
                continue
            i = got[0]
            try:
                rd, wr = i.regs_access()
            except Exception:
                rd, wr = (), ()
            reads = {n for n in map(self.regnum, rd) if n is not None}
            writes = {n for n in map(self.regnum, wr) if n is not None}
            w = self.word(off)
            if (w & 0x1f800000) == 0x13000000 and (w >> 29) & 3 != 1 and (w & 31) != (w >> 5) & 31:
                reads.discard(w & 31)       # capstone lists Rd of sbfm/ubfm aliases as read
            self.ins[off] = (i.mnemonic, i.op_str, reads, writes)

    # ---- switch jump tables --------------------------------------------
    def last_def(self, off, reg, limit=400, fallthrough_only=False):
        """Offset of the closest earlier instruction that writes reg.

        fallthrough_only stops at an unconditional branch (off is not reached from it)."""
        o = off - 4
        while o >= 0 and off - o <= limit * 4:
            info = self.ins.get(o)
            if info and reg in info[3]:
                return o
            w = self.word(o)
            if fallthrough_only and (is_ret_or_br(w) or (w & 0xfc000000) == 0x14000000):
                return None
            if o in self.call_targets:
                return None
            o -= 4
        return None

    def address_in(self, off, reg):
        """Address put in reg by adr / adrp+add before off, or None."""
        o = self.last_def(off, reg)
        if o is None:
            return None
        if self.ins[o][0] == "adr":
            return self.refs[o][1]
        if o in self.lo12 and self.lo12[o][0] == "add":
            return self.lo12[o][1]
        return None

    def find_jump_tables(self):
        """Recover compiler switch tables: entry = ldr{b,h,sw}[table, idx]; target = anchor + entry << k."""
        tables, unknown = {}, []
        for off in sorted(self.ins):
            w = self.word(off)
            if (w & 0xfffffc1f) != 0xd61f0000:
                continue
            reg = (w >> 5) & 31
            found = None
            add = self.last_def(off, reg, 8)
            m = add is not None and self.ins[add][0] == "add" and re.match(
                r"x(\d+), x(\d+), [wx](\d+)(?:, (lsl|sxtb|sxth|sxtw|uxtw|uxtb|uxth) #(\d+))?$", self.ins[add][1])
            if m:
                anchor = self.address_in(add, int(m.group(2)))
                shift = int(m.group(5) or 0)
                ext = m.group(4) or ""
                lo = self.last_def(add, int(m.group(3)), 16)
                lm = lo is not None and re.match(r"[wx]\d+, \[x(\d+), [wx](\d+)", self.ins[lo][1])
                kind = self.ins[lo][0] if lo is not None else ""
                table = self.address_in(lo, int(lm.group(1))) if lm else None
                bound = None
                for o in range(off - 4, off - 160, -4):
                    if o not in self.ins:
                        continue
                    mn = self.ins[o][0]
                    if mn in ("b.hi", "b.hs"):
                        for c in range(o - 4, o - 40, -4):
                            if self.ins.get(c, ("",))[0] == "cmp":
                                mm = re.match(r"[wx]\d+, #(0x[0-9a-f]+|\d+)$", self.ins[c][1])
                                if mm:
                                    bound = int(mm.group(1), 0) + (1 if mn == "b.hi" else 0)
                                break
                        break
                if anchor is not None and table is not None and bound and kind in (
                        "ldrb", "ldrsb", "ldrh", "ldrsh", "ldrsw", "ldr") and shift in (0, 2):
                    size = {"ldrb": 1, "ldrsb": 1, "ldrh": 2, "ldrsh": 2}.get(kind, 4)
                    signed = kind in ("ldrsb", "ldrsh", "ldrsw") or ext.startswith("sxt")
                    fmt = {1: "b", 2: "h", 4: "i"}[size]
                    fmt = fmt if signed else fmt.upper()
                    ents = [struct.unpack_from("<" + fmt, self.d, table + i * size)[0] for i in range(bound)]
                    found = {"br": off, "table": table, "anchor": anchor, "entries": bound, "size": size,
                             "shift": shift, "targets": sorted({anchor + (e << shift) for e in ents})}
            if found:
                tables[off] = found
            else:
                unknown.append(off)
        return tables, unknown

    # ---- adrp pairing -------------------------------------------------
    def trace_page(self, start, reg):
        """Follow the page held in reg after the adrp at start; return users and escapes."""
        users, escapes = [], []
        seen, work = set(), [start + 4]
        steps = 0
        while work:
            off = work.pop()
            while TEXT <= off < RODATA and off not in seen:
                seen.add(off)
                steps += 1
                if steps > 20000:
                    escapes.append(("budget", off))
                    return users, escapes
                w = self.word(off)
                info = self.ins.get(off)
                if info is None:
                    break
                _, _, reads, writes = info
                user = lo12_user(w)
                if user and user[1] == reg:
                    users.append((off, user[0], user[2]))
                    if reg in writes:
                        break
                elif reg in reads:
                    escapes.append(("read", off))
                if reg in writes:
                    break
                if is_call(w) and (reg <= 18 or reg == 30):
                    break
                if off in self.jump_tables:
                    work.extend(self.jump_tables[off]["targets"])
                    break
                if is_ret_or_br(w):
                    break
                ref = decode_ref(w, off)
                if ref and ref[0] == "b":
                    if ref[1] in self.call_targets:
                        break
                    off = ref[1]
                    continue
                if ref and ref[0] in ("bcond", "cbz", "tbz") and ref[1] not in self.call_targets:
                    work.append(ref[1])
                off += 4
                if off in self.call_targets:     # fell off the end (after a noreturn call)
                    break
        return users, escapes

    def pair_adrp(self):
        refs, literal = {}, {}            # text offset -> (kind, target[, size]); pool offset -> size
        for off in range(TEXT, RODATA, 4):
            ref = decode_ref(self.word(off), off)
            if ref is None or off not in self.ins:
                continue
            refs[off] = ref
            if ref[0] == "ldrlit" and TEXT <= ref[1] < RODATA:
                literal[ref[1]] = max(literal.get(ref[1], 0), ref[2] or 4)
        lit_words = {k for off, size in literal.items() for k in range(off, off + max(size, 4), 4)}
        for k in lit_words:               # literal-pool words are data, not instructions
            refs.pop(k, None)
        self.refs, self.literal, self.lit_words = refs, literal, lit_words

        lo12, adrp_sym, adrp_users = {}, {}, {}   # user -> (kind, target); adrp -> symbol target, users
        for off, ref in refs.items():
            if ref[0] != "adrp":
                continue
            users, escapes = self.trace_page(off, self.word(off) & 31)
            adrp_users[off] = [u[0] for u in users]
            targets = []
            for uoff, kind, byte in users:
                target = ref[1] + byte
                if uoff in lo12 and lo12[uoff][1] != target:
                    self.notes["user_conflict"].append(uoff)
                lo12[uoff] = (kind, target)
                targets.append(target)
            escapes = [e for e in escapes if (off, e[1]) not in REVIEWED_ESCAPES]
            if escapes:
                self.notes["adrp_escape"].append([off] + [e[1] for e in escapes][:4])
            if not targets:
                self.notes["adrp_no_user"].append(off)
                adrp_sym[off] = ref[1]
                continue
            if len(set(targets)) > 1:
                where = sorted({section_of(t) for t in targets})
                self.notes["adrp_shared_page_" + "+".join(where)].append(off)
            adrp_sym[off] = targets[0]
        self.lo12, self.adrp_sym, self.adrp_users = lo12, adrp_sym, adrp_users

    def analyse(self):
        self.disassemble()
        self.call_targets = {decode_ref(self.word(o), o)[1] for o in self.ins
                             if (self.word(o) & 0xfc000000) == 0x94000000}
        self.lo12, self.jump_tables = {}, {}
        self.pair_adrp()
        for _ in range(2):                # tables need the adrp pairs; the pairs then follow the tables
            self.jump_tables, unknown_br = self.find_jump_tables()
            self.notes.clear()
            self.pair_adrp()
        self.notes["br_not_table"] = unknown_br
        hi = BASE + BSS_END

        # Absolute pointers in rodata, data and literal pools.
        pointers = {}
        for start, end in ((RODATA, DATA), (DATA, FILE_END)):
            for off in range(start, end - 7, 8):
                v = self.quad(off)
                if BASE <= v <= hi:
                    pointers[off] = v - BASE
            for off in range(start, end - 7):
                if off % 8 and BASE <= self.quad(off) <= hi:
                    self.notes["unaligned_pointer"].append(off)
        for off, size in self.literal.items():
            if size == 8 and BASE <= self.quad(off) <= hi:
                pointers[off] = self.quad(off) - BASE
        self.pointers = pointers

        # Absolute addresses built with movz/movk would not move with the code; list them.
        regs = {}
        phys = BASE ^ 0xffff000000000000
        for off in range(TEXT, RODATA, 4):
            w = self.word(off)
            rd, hw, imm = w & 31, (w >> 21) & 3, (w >> 5) & 0xffff
            if (w & 0xff800000) == 0xd2800000:                  # movz x
                regs[rd] = imm << (16 * hw)
            elif (w & 0xff800000) == 0xf2800000 and rd in regs:  # movk x
                regs[rd] = (regs[rd] & ~(0xffff << (16 * hw))) | (imm << (16 * hw))
            else:
                for r in self.ins.get(off, (0, 0, set(), set()))[3]:
                    regs.pop(r, None)
                continue
            if BASE <= regs[rd] <= hi:
                self.notes["movz_image_address"].append(off)
            if phys <= regs[rd] <= phys + BSS_END:
                self.notes["movz_image_phys_address"].append(off)

        # Jump-table entries stay raw bytes: anchor and targets must sit in one chunk (checked in plan).
        self.notes["out_of_image"] = [o for o, r in self.refs.items()
                                      if r[0] != "adrp" and not (0 <= r[1] <= BSS_END)] + \
                                     [o for o, t in self.adrp_sym.items() if not (0 <= t <= BSS_END)]

    # ---- labels and chunks -------------------------------------------
    def targets(self):
        t = defaultdict(set)      # target offset -> set of referencing chunk keys ("data" or chunk start)
        for off, ref in self.refs.items():
            if ref[0] != "adrp" and 0 <= ref[1] <= BSS_END:
                t[ref[1]].add(("text", off))
        for off, target in self.adrp_sym.items():
            if 0 <= target <= BSS_END:
                t[target].add(("text", off))
        for off, (_, target) in self.lo12.items():
            t[target].add(("text", off))
        for off, target in self.pointers.items():
            t[target].add(("data", off) if off >= RODATA else ("text", off))
        for off in self.user_names:
            t[off].add(("data", -1))      # a name makes the label global
        return t

    def plan(self):
        tg = self.targets()
        starts = {0}
        for off, ref in self.refs.items():
            if ref[0] == "bl" and TEXT <= ref[1] < RODATA:
                starts.add(ref[1])
        anchors = {j["anchor"] for j in self.jump_tables.values()}
        for target, users in tg.items():
            if TEXT <= target < RODATA and target not in self.lit_words and target not in anchors:
                if any(kind == "data" for kind, _ in users) or any(
                        self.refs.get(o, ("",))[0] in ("adr",) or o in self.lo12 or o in self.adrp_sym
                        for kind, o in users if kind == "text"):
                    starts.add(target)
        starts = sorted(s for s in starts if s % 4 == 0)
        # Trailing zero padding of text goes to the linker's ALIGN, not to the last chunk.
        text_end = RODATA
        while text_end > starts[-1] + 4 and self.word(text_end - 4) == 0 and (text_end - 4) not in tg:
            text_end -= 4
        rodata_end = DATA
        while rodata_end > RODATA and self.d[rodata_end - 1] == 0 and (rodata_end - 1) not in tg:
            rodata_end -= 1
        self.text_end, self.rodata_end = text_end, rodata_end
        self.starts = starts
        chunk_of = {}
        bounds = starts + [text_end]
        for a, b in zip(bounds, bounds[1:]):
            chunk_of[a] = (a, b)
        self.chunks = [chunk_of[s] for s in starts]

        def chunk_key(off):
            i = 0
            lo, hi = 0, len(starts) - 1
            while lo <= hi:
                mid = (lo + hi) // 2
                if starts[mid] <= off:
                    i, lo = mid, mid + 1
                else:
                    hi = mid - 1
            return starts[i]

        names, glob = {}, set()
        start_set = set(starts)
        for target, users in tg.items():
            sec = section_of(target) if target < BSS_END else "bss"
            if target == BSS_END:
                sec = "bss"
            if sec == "text":
                if target >= text_end:
                    names[target] = f"text_end_{target:06x}"
                    glob.add(target)
                    continue
                if target in start_set:
                    names[target] = f"f_{target:06x}"
                else:
                    names[target] = f".L_{target:06x}"
                    home = chunk_key(target)
                    if any(kind == "data" or chunk_key(o) != home for kind, o in users):
                        names[target] = f"L_{target:06x}"
                        glob.add(target)
            else:
                names[target] = {"rodata": "r", "data": "d", "bss": "b"}[sec] + f"_{target:06x}"
                glob.add(target)
        for s in starts:
            names[s] = f"f_{s:06x}"
            glob.add(s)
        self.names, self.glob, self.chunk_key = names, glob, chunk_key
        # "nop; adr" pairs are adrp+add pairs that the stock linker relaxed. They are
        # written as the adr_far macro: the stock bytes in the stock layout, adrp+add otherwise.
        self.far_adr = {}
        for off, ref in self.refs.items():
            if ref[0] == "adr" and self.word(off - 4) == NOP and (off - 4) in self.ins and \
                    off not in self.names and (off - 4) not in self.lit_words and \
                    self.chunk_key(off - 4) == self.chunk_key(off):
                self.far_adr[off] = ref[1]
        # Exception vector tables (VBAR_EL1 targets) need 2 KiB alignment; keep the
        # chunks that hold them 2 KiB aligned. Other chunks keep up to 16-byte alignment.
        self.align = {}
        for a, _ in self.chunks:
            low = (a & -a).bit_length() - 1 if a else 12
            self.align[a] = low if a == 0 else min(low, 4)
        self.vbar_targets = []
        for off in self.ins:
            w = self.word(off)
            if (w & 0xffffffe0) != 0xd518c000:          # msr vbar_el1, xN
                continue
            reg = w & 31
            for back in range(off - 4, off - 20, -4):
                ref = self.refs.get(back)
                if ref and ref[0] == "adr" and (self.word(back) & 31) == reg:
                    self.vbar_targets.append(ref[1])
                    break
                if ref and ref[0] == "ldrlit" and (self.word(back) & 31) == reg and ref[1] in self.pointers:
                    self.vbar_targets.append(self.pointers[ref[1]])
                    break
        for jt in self.jump_tables.values():
            c = chunk_key(jt["br"])
            if any(chunk_key(t) != c for t in jt["targets"] + [jt["anchor"]]):
                self.notes["jump_table_split"].append(jt["br"])
        for t in self.vbar_targets:
            c = chunk_key(t)
            assert (t - c) % 0x800 == 0 and (c & -c) >= 0x800, f"vector table {t:#x} in chunk {c:#x}"
            self.align[c] = max(self.align[c], 11)
        for off, (count, _) in self.hooks.items():
            span = range(off, off + 4 * count, 4)
            c = chunk_key(off)
            problems = []
            if not (TEXT <= off and off + 4 * count <= self.text_end) or chunk_key(span[-1]) != c:
                problems.append("outside one chunk")
            if any(o in names for o in span[1:]):
                problems.append("a label inside the replaced words")
            if any(o in self.lit_words for o in span):
                problems.append("literal-pool data")
            if any((o in self.far_adr and o - 4 not in span) or (o + 4 in self.far_adr and o + 4 not in span)
                   for o in span):
                problems.append("half of an adr_far pair")
            if any(chunk_key(j["br"]) == c for j in self.jump_tables.values()):
                problems.append("chunk has a switch jump table")
            for o in span:
                if o in self.adrp_users and any(u not in span for u in self.adrp_users[o]):
                    problems.append(f"adrp at {o:#x} has users outside the hook")
            if problems:
                sys.exit(f"hook at {off:#x}: " + ", ".join(problems))

    # ---- emission ----------------------------------------------------
    def sym(self, target):
        return self.names[target]

    def insn_text(self, off):
        w = self.word(off)
        mn, ops, _, _ = self.ins[off]
        if off in self.fallback:
            return f".inst 0x{w:08x}  // {mn} {ops}"
        ref = self.refs.get(off)
        if ref and ref[0] != "adrp" and 0 <= ref[1] <= BSS_END:
            head = ops.rsplit(", ", 1)[0] + ", " if ", " in ops else ""
            return f"{mn} {head}{self.sym(ref[1])}"
        if off in self.adrp_sym and 0 <= self.adrp_sym[off] <= BSS_END:
            return f"adrp {ops.split(', ')[0]}, {self.sym(self.adrp_sym[off])}"
        if off in self.lo12:
            kind, target = self.lo12[off]
            s = self.sym(target)
            if kind == "add":
                rd, rn = w & 31, (w >> 5) & 31
                r = "x" if w >> 31 else "w"
                name = lambda n: (("sp" if r == "x" else "wsp") if n == 31 else f"{r}{n}")
                return f"add {name(rd)}, {name(rn)}, :lo12:{s}"
            new, n = re.subn(r"\[(\w+)(, #[^\]]+)?\]", lambda m: f"[{m.group(1)}, :lo12:{s}]", ops)
            if n != 1:
                return f".inst 0x{w:08x}  // {mn} {ops} (lo12 {s})"
            return f"{mn} {new}"
        if ref or off in self.adrp_sym:      # reference outside the image: keep the bytes
            return f".inst 0x{w:08x}  // {mn} {ops} (outside image)"
        return f"{mn} {ops}".rstrip()

    def emit_text(self, out):
        tdir = out / "text"
        tdir.mkdir(parents=True, exist_ok=True)
        for old in tdir.glob("*.S"):
            old.unlink()
        for a, b in self.chunks:
            lines = [f"// lk stock offset {a:#08x}-{b:#08x}", '\t.include "lk.inc"',
                     f'\t.section .text.lk.{a:06x}, "ax", %progbits',
                     f"\t.p2align {self.align[a]}"]
            off = a
            while off < b:
                if off in self.names:
                    n = self.names[off]
                    if off in self.glob:
                        lines.append(f"\t.globl {n}")
                        if n.startswith("f_"):
                            lines.append(f"\t.type {n}, %function")
                    lines.append(f"{n}:")
                if off in self.hooks:
                    count, insns = self.hooks[off]
                    lines.append(f"\t// t70 hook: replaces {count} stock word(s) at {off:#x}")
                    lines += [f"\t{i}" for i in insns]
                    off += 4 * count
                    continue
                if off in self.lit_words:
                    if off in self.pointers:
                        lines.append(f"\t.quad {self.sym(self.pointers[off])}")
                        off += 8
                        continue
                    if off % 8 == 0 and off + 4 in self.lit_words and (off + 4) not in self.names:
                        lines.append(f"\t.quad 0x{self.quad(off):016x}")
                        off += 8
                        continue
                    lines.append(f"\t.word 0x{self.word(off):08x}")
                    off += 4
                    continue
                # Runs of zero words with no label inside.
                run = off
                while run < b and self.word(run) == 0 and run not in self.lit_words and run not in self.hooks and (
                        run == off or run not in self.names):
                    run += 4
                if run - off >= 16:
                    lines.append(f"\t.zero {run - off:#x}")
                    off = run
                    continue
                if off + 4 in self.far_adr:
                    reg = self.ins[off + 4][1].split(", ")[0]
                    lines.append(f"\tadr_far {reg}, {self.sym(self.far_adr[off + 4])}")
                    off += 8
                    continue
                if off not in self.ins:
                    lines.append(f"\t.inst 0x{self.word(off):08x}  // (not an instruction)")
                else:
                    lines.append("\t" + self.insn_text(off))
                off += 4
            if b == self.text_end and b in self.names:
                lines += [f"\t.globl {self.names[b]}", f"{self.names[b]}:"]
            (tdir / f"{a:06x}.S").write_text("\n".join(lines) + "\n")

    def emit_data(self, out, start, end, name, section, flags):
        lines = [f"// lk stock offset {start:#08x}-{end:#08x}", f'\t.section {section}, "{flags}", %progbits',
                 "\t.p2align 12"]
        off = start
        printable = set(range(0x20, 0x7f)) | {9, 10, 13}

        def label(o):
            if o in self.names:
                lines.append(f"\t.globl {self.names[o]}")
                lines.append(f"{self.names[o]}:")

        def blocked(o):
            return o in self.names or any(p <= o < p + 8 for p in (o - o % 8,) if p in self.pointers)

        while off < end:
            label(off)
            if off in self.pointers:
                lines.append(f"\t.quad {self.sym(self.pointers[off])}")
                off += 8
                continue
            # String: printable run ending in NUL, no label or pointer inside.
            k = off
            while k < end and self.d[k] in printable and (k == off or not blocked(k)) and not (
                    k - k % 8 in self.pointers):
                k += 1
            if k - off >= 4 and k < end and self.d[k] == 0 and not blocked(k):
                text = self.d[off:k].decode("ascii")
                esc = text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace(
                    "\t", "\\t").replace("\r", "\\r")
                lines.append(f'\t.asciz "{esc}"')
                off = k + 1
                continue
            k = off
            while k < end and self.d[k] == 0 and (k == off or not blocked(k)) and (k - k % 8) not in self.pointers:
                k += 1
            if k - off >= 16:
                lines.append(f"\t.zero {k - off:#x}")
                off = k
                continue
            row = []
            k = off
            while k < end and len(row) < 16 and (k == off or not blocked(k)) and (k - k % 8) not in self.pointers:
                row.append(self.d[k])
                k += 1
            lines.append("\t.byte " + ", ".join(f"0x{x:02x}" for x in row))
            off = k
        if end in self.names and end not in (DATA, FILE_END):
            label(end)
        (out / name).write_text("\n".join(lines) + "\n")

    def emit_bss(self, out):
        lines = [f"// lk stock bss {FILE_END:#08x}-{BSS_END:#08x}", '\t.section .bss.lk, "aw", %nobits']
        off = FILE_END
        for target in sorted(t for t in self.names if FILE_END <= t < BSS_END):
            if target > off:
                lines.append(f"\t.space {target - off:#x}")
                off = target
            lines += [f"\t.globl {self.names[target]}", f"{self.names[target]}:"]
        if BSS_END > off:
            lines.append(f"\t.space {BSS_END - off:#x}")
        (out / "bss.S").write_text("\n".join(lines) + "\n")

    def emit_inc(self, out):
        inc = out / "include"
        inc.mkdir(parents=True, exist_ok=True)
        (inc / "lk.inc").write_text("""// Macros for the generated lk source.
// LK_STOCK_LAYOUT=1 reproduces the stock bytes; otherwise the code may move freely.
\t.ifndef LK_INC
\t.set LK_INC, 1
\t.macro adr_far reg, sym
\t.if LK_STOCK_LAYOUT
\tnop
\tadr \\reg, \\sym
\t.else
\tadrp \\reg, \\sym
\tadd \\reg, \\reg, :lo12:\\sym
\t.endif
\t.endm
\t.endif
""")

    def emit_ld(self, out):
        (out / "lk.ld").write_text(f"""/* Layout of the stock T70 Pro G3D1 V1.08 lk (BL33) stage. */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(f_000000)
SECTIONS
{{
    . = {BASE:#x};
    __code_start = .;
    .text : {{ KEEP(*(SORT_BY_NAME(.text.lk.*))) *(.text .text.*) }}
    . = ALIGN(0x1000);
    .rodata : {{ KEEP(*(.rodata.lk)) *(.rodata .rodata.*) }}
    . = ALIGN(0x1000);
    .data : {{ KEEP(*(.data.lk)) *(.data .data.*) }}
    /* Keep the stock bss at its page offset ({FILE_END & 0xfff:#x}) after added data, so
       adrp pairs that share a page and the alignment of bss objects still hold. */
    . = . + (({FILE_END & 0xfff:#x} - .) & 0xfff);
    .bss (NOLOAD) : {{ KEEP(*(.bss.lk)) *(.bss .bss.* COMMON) }}
    /* End of the stock bss: the stock heap starts here, after any added bss. */
    . = ALIGN(0x1000);
    {self.names[BSS_END]} = .;
    _end = .;
    /DISCARD/ : {{ *(.comment) *(.note*) }}
}}
""" + "".join(f"PROVIDE({n} = {self.names[o]});\n" for o, n in sorted(self.user_names.items())))

    def write(self, out):
        out.mkdir(parents=True, exist_ok=True)
        self.emit_text(out)
        self.emit_data(out, RODATA, self.rodata_end, "rodata.S", ".rodata.lk", "a")
        self.emit_data(out, DATA, FILE_END, "data.S", ".data.lk", "aw")
        self.emit_bss(out)
        self.emit_inc(out)
        self.emit_ld(out)
        notes = {k: (v if len(v) < 400 else v[:400] + ["..."]) for k, v in self.notes.items()}
        layout = {
            "input_sha256": hashlib.sha256(self.d).hexdigest(), "base": f"{BASE:#x}",
            "sections": {"text": [TEXT, self.text_end], "rodata": [RODATA, self.rodata_end],
                         "data": [DATA, FILE_END], "bss": [FILE_END, BSS_END]},
            "counts": {"chunks": len(self.chunks), "instructions": len(self.ins), "refs": len(self.refs),
                       "adrp": len(self.adrp_sym), "lo12_users": len(self.lo12), "pointers": len(self.pointers),
                       "literal_pool": len(self.literal), "labels": len(self.names),
                       "fallback": len(self.fallback), "far_adr": len(self.far_adr)},
            "vector_tables": [f"{t:#x}" for t in sorted(self.vbar_targets)],
            "jump_tables": [{"br": f"{j['br']:#x}", "table": f"{j['table']:#x}", "anchor": f"{j['anchor']:#x}",
                             "entries": j["entries"], "size": j["size"], "shift": j["shift"]}
                            for j in sorted(self.jump_tables.values(), key=lambda j: j["br"])],
            "notes_count": {k: len(v) for k, v in self.notes.items()},
            "notes": {k: [f"{x:#x}" if isinstance(x, int) else [f"{y:#x}" for y in x] if isinstance(x, list) else x
                          for x in v] for k, v in notes.items()},
        }
        (out / "layout.json").write_text(json.dumps(layout, indent=1) + "\n")
        return layout


def main(argv):
    args = list(argv)
    fallback = []
    if "--fallback" in args:
        i = args.index("--fallback")
        path = Path(args[i + 1])
        del args[i:i + 2]
        if path.exists():
            fallback = [int(x, 16) for x in path.read_text().split()]
    names, hooks = {}, {}
    for opt in ("--names", "--hooks"):
        if opt in args:
            i = args.index(opt)
            path = Path(args[i + 1])
            del args[i:i + 2]
            for raw in path.read_text().splitlines():
                line = raw.split("#", 1)[0].strip()
                if not line:
                    continue
                if opt == "--names":
                    off, name = line.split()[:2]
                    names[int(off, 16)] = name
                else:
                    head, body = line.split(":", 1)
                    off, count = head.split()
                    hooks[int(off, 16)] = (int(count), [x.strip() for x in body.split(";") if x.strip()])
    src, out = Path(args[0]), Path(args[1])
    data = src.read_bytes()
    if hashlib.sha256(data).hexdigest() != STOCK_SHA or len(data) != FILE_END:
        sys.exit(f"{src} is not the analysed G3D1 V1.08 lk payload")
    lk = Lk(data, fallback, names, hooks)
    lk.analyse()
    lk.plan()
    layout = lk.write(out)
    print(json.dumps({"counts": layout["counts"], "notes_count": layout["notes_count"]}, indent=1))


if __name__ == "__main__":
    main(sys.argv[1:])
