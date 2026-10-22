import sys, struct, bisect
irx, funcs = sys.argv[1], sys.argv[2]
d = open(irx, 'rb').read()
shoff = struct.unpack_from('<I', d, 0x20)[0]; shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2E)
secs = [struct.unpack_from('<10I', d, shoff + i * shentsize) for i in range(shnum)]
strtab = secs[shstrndx][4]
name = lambda s: d[strtab + s[0]:].split(b'\0')[0]
text = next(s for s in secs if name(s) == b'.text'); rod = next(s for s in secs if name(s) == b'.rodata')
toff, tsize = text[4], text[5]; roff, raddr, rsize = rod[4], rod[3], rod[5]
starts = [int(l.split()[0], 16) for l in open(funcs)]
counts = {int(l.split()[0], 16): int(l.split()[1]) for l in open(funcs)}
words = [struct.unpack_from('<I', d, toff + i)[0] for i in range(0, tsize, 4)]
seen = set()
for i, w in enumerate(words):
    if (w >> 26) == 0x0f:  # lui
        rt = (w >> 16) & 31; hi = w & 0xffff
        for j in range(i + 1, min(i + 8, len(words))):
            v = words[j]
            if (v >> 26) == 0x09 and ((v >> 21) & 31) == rt:
                lo = v & 0xffff; lo = lo - 0x10000 if lo & 0x8000 else lo
                a = (hi << 16) + lo
                if raddr <= a < raddr + rsize:
                    s = d[roff + a - raddr:].split(b'\0')[0]
                    if len(s) > 6:
                        f = starts[bisect.bisect_right(starts, i * 4) - 1]
                        key = (f, s)
                        if key not in seen:
                            seen.add(key); print('%#06x calls=%-7d %s' % (f, counts.get(f, 0), s.decode(errors='replace')[:70]))
                break
