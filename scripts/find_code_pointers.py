"""Find function starts referenced from data that the function map missed.

usage: python3 -I find_code_pointers.py <elf> <register_functions.cpp> > hints.txt

A data word is a candidate when it points into .text at a 4-byte aligned
address that directly follows a function return (jr $ra + delay slot,
optionally followed by nop padding) and is not already a function start.
"""
import re
import struct
import sys

JR_RA = 0x03E00008


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
    known = {int(m, 16) for m in re.findall(r"// 0x([0-9a-f]+)$", open(sys.argv[2]).read(), re.M)}
    known |= {int(m, 16) for m in re.findall(r"_0x([0-9a-f]+)\b", open(sys.argv[2]).read())}
    secs = sections(elf)
    _, text_addr, text_off, text_size = secs[".text"]

    def word_at(address):
        return struct.unpack_from("<I", elf, text_off + address - text_addr)[0]

    def follows_return(target):
        probe = target - 4
        while probe >= text_addr + 8 and word_at(probe) == 0 and target - probe <= 16:
            if word_at(probe - 4) == JR_RA:
                return True
            probe -= 4
        return word_at(target - 8) == JR_RA

    found = set()
    for name in (".data", ".rodata", ".sdata"):
        stype, addr, off, size = secs[name]
        if stype == 8:
            continue
        for pos in range(0, size - 3, 4):
            value = struct.unpack_from("<I", elf, off + pos)[0]
            if text_addr + 8 <= value < text_addr + text_size and value % 4 == 0:
                if value not in known and word_at(value) != 0 and follows_return(value):
                    found.add(value)
    for value in sorted(found):
        print('  "sub_%08X@0x%08X",' % (value, value))
    print("%d candidates" % len(found), file=sys.stderr)


main()
