"""Static MSVC RTTI resolver for x64 PE files such as Soulash 2.exe. Read-only.

Maps a class name to its vtable RVA, lists the virtual functions (with their
.pdata ranges) and the functions that load the vtable address (constructors).
This is the offline twin of the extender's findRttiVtable anchor.

  python -I tools/pe_rtti.py <exe> <ClassName|.?AV...@@|.?AU...@@> [...]

A bare name is tried as a class (".?AV<name>@@"); pass the decorated form for
structs (".?AUAbilityUser@@") or anonymous-namespace types. Results are only
valid for the exact executable they were produced from.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_xrefs import Pe, fmt_func  # noqa: E402


def in_sec(rva, sec):
    va, vs, raw, rs = sec
    return va <= rva < va + max(vs, rs)


def find_all(blob, needle, base):
    i = 0
    while True:
        i = blob.find(needle, i)
        if i < 0:
            return
        yield base + i
        i += 1


def qwords(data, sec):
    va, vs, raw, rs = sec
    blob = data[raw:raw + rs]
    for off in range(0, len(blob) - 7, 8):
        yield va + off, struct.unpack_from("<Q", blob, off)[0]


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    pe = Pe(sys.argv[1])
    d = pe.data
    pe_header = struct.unpack_from("<I", d, 0x3C)[0]
    image_base = struct.unpack_from("<Q", d, pe_header + 24 + 24)[0]
    text = pe.secs[".text"]
    data_secs = [pe.secs[n] for n in (".rdata", ".data") if n in pe.secs]
    scan = list(pe.scan_text())

    for name in sys.argv[2:]:
        decorated = name if name.startswith(".?A") else f".?AV{name}@@"
        descriptors = []
        for sec in data_secs:
            va, vs, raw, rs = sec
            blob = d[raw:raw + rs]
            for hit in find_all(blob, decorated.encode() + b"\0", va):
                descriptors.append(hit - 16)  # TypeDescriptor: vtable(8) spare(8) name[]
        print(f"## {name}: type descriptors at {[hex(t) for t in descriptors]}")
        for td in descriptors:
            # CompleteObjectLocator (x64): sig=1, offset, cdOffset, pTypeDescriptor,
            # pClassDescriptor, pSelf -- all image-relative 32-bit values.
            locators = []
            for sec in data_secs:
                va, vs, raw, rs = sec
                blob = d[raw:raw + rs]
                for hit in find_all(blob, struct.pack("<I", td), va):
                    col = hit - 12
                    if col < va:
                        continue
                    sig, this_offset, _cd = struct.unpack_from("<III", d, raw + (col - va))
                    p_self = struct.unpack_from("<I", d, raw + (col - va) + 20)[0]
                    if sig == 1 and p_self == col:
                        locators.append((col, this_offset))
            for col, this_offset in locators:
                for sec in data_secs:
                    for slot, value in qwords(d, sec):
                        if value != image_base + col:
                            continue
                        vtable = slot + 8
                        print(f"  COL {col:#x} (this-offset {this_offset}) -> vtable {vtable:#x}")
                        i = 0
                        while i <= 80:
                            fn = struct.unpack_from("<Q", d, pe.off(vtable + i * 8))[0] - image_base
                            if not in_sec(fn, text):
                                break
                            print(f"    [{i:2d}] {fn:#x} {fmt_func(pe, fn)}")
                            i += 1
                        users = sorted({(pe.func_of(r) or (r, r))[0]
                                        for r, kind, tgt in scan if tgt == vtable and kind == "lea"})
                        print(f"    vtable loaded (lea) by functions: {[hex(u) for u in users]}")


if __name__ == "__main__":
    main()
