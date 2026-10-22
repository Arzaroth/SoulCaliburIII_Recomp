import sys, struct, subprocess, collections, re
prof, binary = sys.argv[1], sys.argv[2]
d = open(prof, 'rb').read()
w = lambda i: struct.unpack_from('<Q', d, i * 8)[0]
hdr = w(1); i = 2 + hdr - 0  # header words: 0, hdrcount, version, period, padding
i = 5
flat = collections.Counter(); cum = collections.Counter(); total = 0
samples = []
while True:
    count, depth = w(i), w(i + 1)
    pcs = [w(i + 2 + k) for k in range(depth)]
    i += 2 + depth
    if count == 0 and depth == 1 and pcs[0] == 0:
        break
    samples.append((count, pcs))
maps = d[i * 8:].decode(errors='replace')
base = None
for line in maps.splitlines():
    m = re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \S+\s+(\S+)', line)
    if m and m.group(5).endswith(binary.split('/')[-1]) and base is None:
        base = int(m.group(1), 16) - int(m.group(4), 16)
addrs = set()
for c, pcs in samples:
    for p in pcs: addrs.add(p)
rel = {a: a - base for a in addrs if base and a >= base and a - base < 0x40000000}
inp = '\n'.join(hex(v) for v in rel.values())
out = subprocess.run(['llvm-symbolizer', '--obj=' + binary, '--functions=short', '--no-inlines', '--output-style=GNU'], input=inp, capture_output=True, text=True).stdout.split('\n')
names = {}
for (a, v), k in zip(rel.items(), range(len(rel))):
    names[a] = out[k * 2] if k * 2 < len(out) else '?'
for c, pcs in samples:
    total += c
    if pcs: flat[names.get(pcs[0], '?')] += c
    seen = set()
    for p in pcs:
        n = names.get(p, '?')
        if n not in seen: cum[n] += c; seen.add(n)
print('total', total)
for n, c in flat.most_common(30): print('%5.1f%% flat  %5.1f%% cum  %s' % (100 * c / total, 100 * cum[n] / total, n[:90]))
