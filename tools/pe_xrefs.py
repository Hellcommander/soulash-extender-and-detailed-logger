"""Static, read-only cross-reference helper for x64 PE files such as Soulash 2.exe.

It never loads or modifies the executable. Run with `python -I` so nothing is
imported from the current directory:

  python -I tools/pe_xrefs.py <exe> iat   <slot-rva> [...]     # name the import at an IAT slot
  python -I tools/pe_xrefs.py <exe> calls <import-substring>   # call sites of matching imports
  python -I tools/pe_xrefs.py <exe> refs  <rva> [...]          # call/jmp/lea references to RVAs
  python -I tools/pe_xrefs.py <exe> str   <ascii-substring>    # strings + the functions using them
  python -I tools/pe_xrefs.py <exe> fnstr <string> [...]       # all strings in functions using <string> (parser key sets)
  python -I tools/pe_xrefs.py <exe> has   <key> [...]          # is a JSON key/effect name known to the exe?

Addresses are RVAs (image base excluded) so they survive ASLR. Results are only
valid for the exact executable they were produced from (see docs/game-notes.md,
which keys everything by SHA-256).
"""
import bisect
import re
import struct
import sys


class Pe:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        if struct.unpack_from("<H", d, opt)[0] != 0x20B:
            raise SystemExit("not a PE32+ (x64) image")
        self.entry = struct.unpack_from("<I", d, opt + 16)[0]
        self.dirs = opt + 112
        self.secs = {}
        for i in range(nsec):
            o = opt + optsz + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode()
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, o + 8)
            self.secs[name] = (va, vsize, raw, rsize)
        self._imports()
        self._functions()

    def off(self, rva):
        for va, vs, raw, rs in self.secs.values():
            if va <= rva < va + max(vs, rs):
                return raw + rva - va
        raise ValueError(hex(rva))

    def cstr(self, rva):
        o = self.off(rva)
        return self.data[o:self.data.index(b"\0", o)].decode("latin1")

    def _imports(self):
        self.iat = {}
        rva, _ = struct.unpack_from("<II", self.data, self.dirs + 8)
        p = self.off(rva)
        while True:
            oft, _ts, _fc, name_rva, first = struct.unpack_from("<IIIII", self.data, p)
            if name_rva == 0:
                break
            dll = self.cstr(name_rva)
            t = self.off(oft or first)
            i = 0
            while True:
                v = struct.unpack_from("<Q", self.data, t + i * 8)[0]
                if v == 0:
                    break
                nm = f"#{v & 0xffff}" if v >> 63 else self.cstr((v & 0x7fffffff) + 2)
                self.iat[first + i * 8] = f"{dll}!{nm}"
                i += 1
            p += 20

    def _functions(self):
        va, vs, raw, _ = self.secs[".pdata"]
        self.funcs = []
        for i in range(vs // 12):
            b, e, _u = struct.unpack_from("<III", self.data, raw + i * 12)
            if b == 0:
                break
            self.funcs.append((b, e))
        self.starts = [f[0] for f in self.funcs]

    def func_of(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            return self.funcs[i]
        return None

    def scan_text(self):
        """Yield (rva, kind, target_rva) for direct call/jmp, lea and rip-relative loads/calls."""
        va, vs, raw, _ = self.secs[".text"]
        body = self.data[raw:raw + vs]
        for i in range(len(body) - 7):
            b = body[i]
            if b in (0xE8, 0xE9):
                d = struct.unpack_from("<i", body, i + 1)[0]
                yield va + i, "call" if b == 0xE8 else "jmp", va + i + 5 + d
            elif b == 0xFF and body[i + 1] in (0x15, 0x25):
                d = struct.unpack_from("<i", body, i + 2)[0]
                yield va + i, "call*" if body[i + 1] == 0x15 else "jmp*", va + i + 6 + d
            elif b in (0x48, 0x4C) and body[i + 1] in (0x8B, 0x8D) and (body[i + 2] & 0xC7) == 0x05:
                d = struct.unpack_from("<i", body, i + 3)[0]
                yield va + i, "lea" if body[i + 1] == 0x8D else "load", va + i + 7 + d


def fmt_func(pe, rva):
    f = pe.func_of(rva)
    return f"func {f[0]:#x}-{f[1]:#x}" if f else "no func"


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    pe = Pe(sys.argv[1])
    mode, args = sys.argv[2], sys.argv[3:]

    if mode == "iat":
        for a in args:
            slot = int(a, 16)
            print(f"{slot:#x}: {pe.iat.get(slot, '?')}")
    elif mode == "calls":
        wanted = {s for s, n in pe.iat.items() if any(a.lower() in n.lower() for a in args)}
        for rva, kind, tgt in pe.scan_text():
            if tgt in wanted and kind in ("call*", "jmp*", "load"):
                print(f"{pe.iat[tgt]:60s} {kind:5s} at {rva:#x} {fmt_func(pe, rva)}")
    elif mode == "refs":
        targets = {int(a, 16) for a in args}
        for rva, kind, tgt in pe.scan_text():
            if tgt in targets:
                print(f"{tgt:#x} <- {kind} at {rva:#x} {fmt_func(pe, rva)}")
    elif mode == "str":
        needle = args[0].encode()
        strings = {}
        for name in (".rdata", ".data"):
            va, _vs, raw, rs = pe.secs[name]
            blob = pe.data[raw:raw + rs]
            for m in re.finditer(re.escape(needle), blob):
                s, e = m.start(), m.end()
                while s > 0 and 32 <= blob[s - 1] < 127:
                    s -= 1
                while e < len(blob) and 32 <= blob[e] < 127:
                    e += 1
                strings[va + s] = blob[s:e].decode("latin1")
        users = {}
        for rva, kind, tgt in pe.scan_text():
            if tgt in strings and kind == "lea":
                f = pe.func_of(rva)
                users.setdefault(tgt, set()).add(f[0] if f else rva)
        for rva, text in sorted(strings.items()):
            fs = ", ".join(f"{x:#x}" for x in sorted(users.get(rva, ())))
            print(f"{rva:#x} {text[:90]!r} used by: {fs or '-'}")
    elif mode == "fnstr":
        # For each function that references <string>, list every identifier-like
        # string literal that function references: parsers show their whole key set.
        data_sections = [pe.secs[n] for n in (".rdata", ".data")]
        per_function = {}
        for rva, kind, tgt in pe.scan_text():
            if kind != "lea" or not any(va <= tgt < va + max(vs, rs) for va, vs, _r, rs in data_sections):
                continue
            func = pe.func_of(rva)
            if not func:
                continue
            try:
                start = pe.off(tgt)
                text = pe.data[start:pe.data.index(b"\0", start)]
            except Exception:
                continue
            if 2 <= len(text) <= 60 and re.fullmatch(rb"[A-Za-z0-9_]+", text):
                per_function.setdefault(func[0], []).append(text.decode())
        for wanted in args:
            hits = sorted(f for f, names in per_function.items() if wanted in names)
            print(f"'{wanted}' is referenced by: {[hex(h) for h in hits]}")
            for h in hits[:3]:
                names = list(dict.fromkeys(per_function[h]))
                print(f"  func {h:#x} ({len(names)} strings): {', '.join(names)}")
    elif mode == "has":
        # Is <key> known to the executable? Strings of 15 characters or fewer are
        # often compiled into code as immediates, so both forms are checked.
        text_va, text_vs, text_raw, _ = pe.secs[".text"]
        code = pe.data[text_raw:text_raw + text_vs]
        for key in args:
            raw_key = key.encode()
            in_data = (b"\0" + raw_key + b"\0") in pe.data
            parts, i = [], 0
            while i < len(raw_key):
                size = next(s for s in (8, 4, 2, 1) if len(raw_key) - i >= s)
                parts.append(raw_key[i:i + size])
                i += size
            inline = len(raw_key) >= 4 and all(code.find(p) != -1 for p in parts if len(p) >= 4)
            print(f"{key:36s} data_string={in_data!s:5s} inline_immediates={inline}")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
