"""Find the functions that build the address of a string.

usage: python3 -I string_refs.py <elf> <generated output dir> <substring>...

For each string containing a substring, scan .text for lui rt,hi followed within
eight instructions by addiu/ori rX,rt,lo that rebuild its address, and report the
generated function (sub_XXXXXXXX file) that contains the reference.
"""
import bisect
import os
import re
import struct
import sys


def sections(elf):
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    raw = [struct.unpack_from("<IIIIIIIIII", elf, shoff + i * shentsize) for i in range(shnum)]
    strtab = raw[shstrndx][4]
    out = {}
    for name, stype, _flags, addr, off, size, *_ in raw:
        end = elf.index(b"\0", strtab + name)
        out[elf[strtab + name:end].decode()] = (stype, addr, off, size)
    return out


def main():
    elf = open(sys.argv[1], "rb").read()
    starts = sorted(int(m.group(1), 16) for f in os.listdir(sys.argv[2])
                    for m in [re.match(r"sub_([0-9A-F]{8})_", f)] if m)
    secs = sections(elf)
    _, text_addr, text_off, text_size = secs[".text"]
    words = struct.unpack_from("<%dI" % (text_size // 4), elf, text_off)

    targets = {}
    for name in (".rodata", ".data"):
        _, addr, off, size = secs[name]
        blob = elf[off:off + size]
        for needle in sys.argv[3:]:
            for m in re.finditer(re.escape(needle.encode()), blob):
                begin = blob.rfind(b"\0", 0, m.start()) + 1
                end = blob.index(b"\0", m.start())
                targets[addr + begin] = blob[begin:end].decode("latin-1")

    for index, word in enumerate(words):
        if word >> 26 != 0x0F:
            continue
        rt, hi = (word >> 16) & 31, word & 0xFFFF
        for follow in words[index + 1:index + 9]:
            op, rs, imm = follow >> 26, (follow >> 21) & 31, follow & 0xFFFF
            if rs != rt or op not in (0x09, 0x0D):
                continue
            value = ((hi << 16) + (imm - 0x10000 if op == 0x09 and imm & 0x8000 else imm)) & 0xFFFFFFFF
            if value in targets:
                pc = text_addr + index * 4
                owner = starts[bisect.bisect_right(starts, pc) - 1] if starts and pc >= starts[0] else 0
                print("0x%08X in sub_%08X  %r" % (pc, owner, targets[value][:70]))
            break


main()
