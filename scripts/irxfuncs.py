import sys, struct
irx, base, cov = sys.argv[1], int(sys.argv[2], 0), sys.argv[3]
d = open(irx, 'rb').read()
shoff = struct.unpack_from('<I', d, 0x20)[0]; shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2E)
secs = [struct.unpack_from('<10I', d, shoff + i * shentsize) for i in range(shnum)]
strtab = secs[shstrndx][4]
text = next(s for s in secs if d[strtab + s[0]:].split(b'\0')[0] == b'.text')
off, size = text[4], text[5]
starts = []
for i in range(0, size, 4):
    w = struct.unpack_from('<I', d, off + i)[0]
    if (w >> 16) == 0x27bd and (w & 0x8000):
        starts.append(i)
counts = {}
for line in open(cov):
    a, c = line.split(); counts[int(a, 16) - base] = int(c)
for s in starts:
    print('%#06x %s' % (s, counts.get(s, 0)))
