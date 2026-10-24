r"""Annotated function listings from a `dumpbin /disasm` dump of an x64 PE. Read-only.

Generate the listing once, outside the repo (about 160 MB for Soulash 2.exe):
  dumpbin /disasm "<exe>" > <scratch>\soulash.asm

  python -I tools/asm_fn.py <exe> <asm> show  <rva> [maxlines]  # function containing rva (pdata range)
  python -I tools/asm_fn.py <exe> <asm> range <rva> <rva-end>   # exact range, across unwind chunks
  python -I tools/asm_fn.py <exe> <asm> grep  <regex> [limit]   # matching instructions grouped by function
  python -I tools/asm_fn.py <exe> <asm> callers <rva>           # direct call/jmp sites, by function
  python -I tools/asm_fn.py <exe> <asm> strs  <rva>             # strings referenced by that function

Annotations: direct branch targets with their containing function, IAT
imports by name, and RIP-relative string operands. MSVC splits one logical
function into several .pdata chunks (prologue, body, epilogue); `show` stops
at the chunk end, `range` does not. Addresses are RVAs (hex, no prefix).
"""
import bisect
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_xrefs import Pe  # noqa: E402

LINE = re.compile(r"^  ([0-9A-F]{16}): (?:[0-9A-F]{2} )*\s*([a-z].*)$")


class Listing:
    def __init__(self, pe, asm_path):
        self.pe = pe
        d = pe.data
        pe_header = struct.unpack_from("<I", d, 0x3C)[0]
        self.image_base = struct.unpack_from("<Q", d, pe_header + 24 + 24)[0]
        self.addrs, self.lines = [], []
        with open(asm_path, "r", encoding="latin1") as f:
            for ln in f:
                m = LINE.match(ln)
                if m:
                    self.addrs.append(int(m.group(1), 16) - self.image_base)
                    self.lines.append(m.group(2).rstrip())

    def string_at(self, rva):
        for name in (".rdata", ".data"):
            if name not in self.pe.secs:
                continue
            va, vs, raw, rs = self.pe.secs[name]
            if va <= rva < va + max(vs, rs):
                try:
                    o = self.pe.off(rva)
                    s = self.pe.data[o:self.pe.data.index(b"\0", o, o + 200)]
                except Exception:
                    return None
                if len(s) >= 2 and all(32 <= c < 127 for c in s):
                    return s.decode("latin1")
        return None

    def annotate(self, text):
        notes = []
        m = re.search(r"\b(call|jmp|j[a-z]+)\s+([0-9A-F]{16})\b", text)
        if m:
            t = int(m.group(2), 16) - self.image_base
            f = self.pe.func_of(t)
            note = f"-> {t:#x}" + (f" (func {f[0]:#x})" if f and f[0] != t else "")
            notes.append(note + (f" {self.pe.iat[t]}" if t in self.pe.iat else ""))
        for m in re.finditer(r"\[([0-9A-F]{16})h?\]", text):
            t = int(m.group(1), 16) - self.image_base
            if t in self.pe.iat:
                notes.append(f"IAT {self.pe.iat[t]}")
            else:
                s = self.string_at(t)
                notes.append(f'"{s[:70]}"' if s else f"[{t:#x}]")
        return "   ; " + " | ".join(notes) if notes else ""

    def func_range(self, rva):
        return self.pe.func_of(rva) or (rva, rva + 0x200)

    def emit(self, start, end, maxlines=None, chunk_headers=False):
        i = bisect.bisect_left(self.addrs, start)
        n = 0
        while i < len(self.addrs) and self.addrs[i] < end and (maxlines is None or n < maxlines):
            a = self.addrs[i]
            f = self.pe.func_of(a)
            if chunk_headers and f and f[0] == a:
                print(f"--- pdata chunk {f[0]:#x}-{f[1]:#x}")
            print(f"{a:#010x}  {self.lines[i]:<60s}{self.annotate(self.lines[i])}")
            i += 1
            n += 1
        if i < len(self.addrs) and self.addrs[i] < end:
            print(f"... ({end - self.addrs[i]:#x} more bytes)")


def main():
    if len(sys.argv) < 5:
        raise SystemExit(__doc__)
    pe = Pe(sys.argv[1])
    listing = Listing(pe, sys.argv[2])
    mode, args = sys.argv[3], sys.argv[4:]

    if mode == "show":
        s, e = listing.func_range(int(args[0], 16))
        print(f"=== function {s:#x}-{e:#x}")
        listing.emit(s, e, int(args[1]) if len(args) > 1 else 400)
    elif mode == "range":
        listing.emit(int(args[0], 16), int(args[1], 16), chunk_headers=True)
    elif mode == "grep":
        rx = re.compile(args[0])
        limit = int(args[1]) if len(args) > 1 else 200
        by_function = {}
        for a, t in zip(listing.addrs, listing.lines):
            if rx.search(t):
                f = pe.func_of(a)
                by_function.setdefault(f[0] if f else a, []).append((a, t))
        print(f"{sum(len(v) for v in by_function.values())} matches in {len(by_function)} functions")
        for f, hits in sorted(by_function.items())[:limit]:
            print(f"func {f:#x}: " + "; ".join(f"{a:#x} {t}" for a, t in hits[:6]) + (" ..." if len(hits) > 6 else ""))
    elif mode == "callers":
        target = int(args[0], 16)
        sites = {}
        for a, t in zip(listing.addrs, listing.lines):
            m = re.search(r"\b(call|jmp)\s+([0-9A-F]{16})\b", t)
            if m and int(m.group(2), 16) - listing.image_base == target:
                f = pe.func_of(a)
                sites.setdefault(f[0] if f else a, []).append(a)
        for f, where in sorted(sites.items()):
            print(f"func {f:#x}: {[hex(x) for x in where]}")
    elif mode == "strs":
        s, e = listing.func_range(int(args[0], 16))
        i = bisect.bisect_left(listing.addrs, s)
        found = []
        while i < len(listing.addrs) and listing.addrs[i] < e:
            for m in re.finditer(r"\[([0-9A-F]{16})h?\]", listing.lines[i]):
                st = listing.string_at(int(m.group(1), 16) - listing.image_base)
                if st and st not in found:
                    found.append(st)
            i += 1
        print(f"function {s:#x}-{e:#x}: {len(found)} strings")
        for st in found:
            print("  " + st[:100])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
