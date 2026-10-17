#!/usr/bin/env python3
"""Extract, verify structurally and dissect the Warden builtin module.

Input: WoW/Source/Game/GameClient/Win32/WardenClient.cpp
Decrypts the stored blob (RC4, fixed key), slices payload the same way LoadModule does,
zlib-inflates the result (image source[4:]. The first 4 bytes of the decrypted prefix are
the uncompressed size (uLongf written by the original code before uncompress).

Then parses the custom BLL image header (uint32 LE):
  [0] imageSize  [2] relocation offset [3] relocation count [4] export table offset
  [5] export count [6] first export ordinal [7] import table offset [8] import count
  [9] section descriptor count; section descriptors start at [10] (offset, length, protect)

Optionally disassembles with capstone if installed.
"""
import re, sys, zlib

def grab(name, src):
    m = re.search(r"static const uint8_t " + name + r"\[[^\]]*\] = \{(.*?)\};", src, re.S)
    return [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1))]

def rc4(key, data):
    S = list(range(256)); j = 0
    for i in range(256):
        j = (j + S[i] + key[i % len(key)]) & 0xFF
        S[i], S[j] = S[j], S[i]
    out = bytearray(); i = j = 0
    for b in data:
        i = (i + 1) & 0xFF
        j = (j + S[i]) & 0xFF
        S[i], S[j] = S[j], S[i]
        out.append(b ^ S[(S[i] + S[j]) & 0xFF])
    return bytes(out)

def make_name(codes):
    name = bytearray(b"M")
    for c in codes:
        ch = (c * 0xB9) & 0xFF
        name.append(ch)
        if not ch:
            break
    return bytes(name).upper()

def cstr(image, off):
    e = image.index(b"\0", off)
    return image[off:e]

def parse(image, want_disasm=False):
    u32 = lambda off: int.from_bytes(image[off:off+4], "little")
    u16 = lambda off: int.from_bytes(image[off:off+2], "little")
    print("== BLL image ==")
    print(f"  imageSize        = {u32(0):#x} ({u32(0)})")
    print(f"  [1] ?            = {u32(4):#x}")
    rel_off, rel_count = u32(8), u32(12)
    exp_off, exp_count, exp_base = u32(16), u32(20), u32(24)
    imp_off, imp_count = u32(28), u32(32)
    desc_count = u32(36)
    print(f"  relocation off   = {rel_off:#x} count {rel_count}")
    print(f"  export   off     = {exp_off:#x} count {exp_count} base {exp_base}")
    print(f"  import   off     = {imp_off:#x} count {imp_count}")
    print(f"  section desc     = {desc_count}")
    sections = []
    for i in range(desc_count):
        off, ln, prot = u32(40+12*i), u32(44+12*i), u32(48+12*i)
        sections.append((off, ln, prot))
        print(f"   sec[{i}] off={off:#010x} len={ln:#09x} prot={prot:#x}")
    print("== imports ==")
    imports = []
    for i in range(imp_count):
        dll_ptr = u32(imp_off + 8*i)
        thunk_ptr = u32(imp_off + 8*i + 4)
        dll = cstr(image, dll_ptr)
        funcs = []
        t = thunk_ptr
        while u32(t):
            v = u32(t)
            if v & 0x80000000:
                funcs.append(f"ordinal {v & 0x7FFFFFFF}")
            else:
                funcs.append(cstr(image, v))
            t += 4
        imports.append((dll, funcs))
        print(f"   {dll}:")
        for f in funcs:
            print(f"     {f}")
    print("== exports ==")
    exports = []
    for i in range(exp_count):
        addr = u32(exp_off + 4*i)
        exports.append(addr)
        print(f"   ordinal {exp_base+i} -> {addr:#x}")
    # relocation decoding stats
    rel = image[rel_off:rel_off + 2048]
    return sections, imports, exports, (rel_off, rel_count)

def disasm(image, exports, imports):
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    except ImportError:
        print("capstone not installed; pip install capstone")
        return
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    print("== disassembly ==")
    for i, addr in enumerate(exports):
        print(f" --- export {i+1} @ {addr:#x} ---")
        code = image[addr:addr+128]
        for ins in md.disasm(code, addr):
            print(f"   {ins.address:#010x}  {ins.mnemonic} {ins.op_str}")
            if ins.mnemonic == "ret":
                break

def main():
    src = open("WoW/Source/Game/GameClient/Win32/WardenClient.cpp").read()
    blob = grab("s_builtinModule", src)
    key = bytes(grab("s_builtinModuleKey", src))
    modulus = bytes(grab("s_moduleModulus", src))
    exponent = bytes(grab("s_moduleExponent", src))
    codes = grab("s_moduleNameCodes", src)
    dec = rc4(key, bytes(blob))
    open(" Tools/Wardendump/dec.bin".strip(), "wb").write(dec)
    declared = int.from_bytes(dec[0:4], "little")
    print("declared uncompressed size:", declared)
    try:
        image = zlib.decompress(dec[4:])
    except zlib.error as e:
        print("zlib plain failed:", e)
        image = zlib.decompressobj(-15).decompress(dec[4:])
    print("image size:", len(image))
    open("/tmp/warden_payload.bin", "wb").write(dec)
    open("Tools/Wardendump/warden_image.bin", "wb").write(image)
    print("== payload tail (signature area) ==")
    print("  payload[-260:-256]:", dec[-260:-256].hex(" "))
    want_disasm = "--disasm" in sys.argv
    parse(image, want_disasm)
    if want_disasm:
        # exports parsed again inside disasm
        u32 = lambda off: int.from_bytes(image[off:off+4], "little")
        exp_off, exp_count = u32(16), u32(20)
        exports = [u32(exp_off + 4*i) for i in range(exp_count)]
        imp_off, imp_count = u32(28), u32(32)
        imports = [(cstr(image, u32(imp_off+8*i)), [u32(imp_off+8*i+4)]) if False else None for i in range(imp_count)]
        disasm(image, exports, imports)

if __name__ == "__main__":
    main()
