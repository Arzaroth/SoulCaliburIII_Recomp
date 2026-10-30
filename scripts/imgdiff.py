#!/usr/bin/env python3
"""Compare two frames the way the regression checks need it, with the standard library only (run it with python3 -I).

usage: scripts/imgdiff.py A B [--diff OUT.png] [--min-psnr DB] [--region X,Y,W,H]
       scripts/imgdiff.py --png IN.rgba OUT.png [--size WxH]

A and B are PNG files (8-bit grey, RGB or RGBA, as the runtime's captures) or raw RGBA frames (PS2X_FRAME_DUMP's
frame_<n>_<w>x<h>.rgba, or any .rgba / .raw with --size). Prints one line of key=value: psnr (dB over RGB, inf when
equal), mean_abs, max_abs, changed (share of pixels with a channel off by more than 8), width, height. --diff writes the
absolute difference, amplified by 4, as a PNG. --min-psnr exits 1 when the PSNR is below it. Images of different sizes
are compared over their common top-left area, and the line says so (cropped=1).

--png converts a raw RGBA frame to a PNG (the regression's pause still).
"""

import argparse
import math
import re
import struct
import sys
import zlib


def read_png(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    pos = 8
    width = height = depth = colour = interlace = None
    idat = []
    palette = None
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            palette = body
        elif kind == b'IDAT':
            idat.append(body)
        elif kind == b'IEND':
            break
    if depth != 8 or interlace != 0 or colour not in (0, 2, 3, 4, 6):
        raise ValueError(f'{path}: only 8-bit non-interlaced PNGs are read (depth {depth}, colour {colour})')
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    raw = zlib.decompress(b''.join(idat))
    stride = width * channels
    out = bytearray(width * height * 4)
    previous = bytearray(stride)
    at = 0
    for y in range(height):
        kind = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        if kind == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xff
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xff
        elif kind == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xff
        elif kind == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = previous[i]
                c = previous[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xff
        previous = line
        row = y * width * 4
        if colour == 6:
            out[row:row + width * 4] = line
            continue
        for x in range(width):
            o = row + x * 4
            if colour == 2:
                out[o:o + 3] = line[x * 3:x * 3 + 3]
                out[o + 3] = 255
            elif colour == 3:
                index = line[x] * 3
                out[o:o + 3] = palette[index:index + 3]
                out[o + 3] = 255
            else:
                grey = line[x * channels]
                out[o] = out[o + 1] = out[o + 2] = grey
                out[o + 3] = line[x * 2 + 1] if colour == 4 else 255
    return width, height, bytes(out)


def raw_size(path, size):
    if size:
        width, height = (int(v) for v in size.lower().split('x'))
        return width, height
    match = re.search(r'(\d+)x(\d+)\.(rgba|raw)$', path)
    if match:
        return int(match.group(1)), int(match.group(2))
    return 640, 448


def read_image(path, size=None):
    with open(path, 'rb') as f:
        head = f.read(8)
    if head == b'\x89PNG\r\n\x1a\n':
        return read_png(path)
    width, height = raw_size(path, size)
    with open(path, 'rb') as f:
        pixels = f.read(width * height * 4)
    if len(pixels) != width * height * 4:
        raise ValueError(f'{path}: {len(pixels)} bytes, not {width}x{height} RGBA')
    return width, height, pixels


def write_png(path, width, height, rgba):
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        rows += rgba[y * width * 4:(y + 1) * width * 4]

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(bytes(rows), 6)))
        f.write(chunk(b'IEND', b''))


def compare(a, b, region, diff_path):
    wa, ha, pa = a
    wb, hb, pb = b
    width, height = min(wa, wb), min(ha, hb)
    x0, y0, x1, y1 = 0, 0, width, height
    if region:
        rx, ry, rw, rh = region
        x0, y0 = max(0, rx), max(0, ry)
        x1, y1 = min(width, rx + rw), min(height, ry + rh)
    squares = 0
    total = 0
    worst = 0
    changed = 0
    diff = bytearray(width * height * 4) if diff_path else None
    for y in range(y0, y1):
        ra = y * wa * 4
        rb = y * wb * 4
        for x in range(x0, x1):
            ia = ra + x * 4
            ib = rb + x * 4
            dr = abs(pa[ia] - pb[ib])
            dg = abs(pa[ia + 1] - pb[ib + 1])
            db = abs(pa[ia + 2] - pb[ib + 2])
            squares += dr * dr + dg * dg + db * db
            total += dr + dg + db
            m = max(dr, dg, db)
            worst = max(worst, m)
            if m > 8:
                changed += 1
            if diff is not None:
                o = (y * width + x) * 4
                diff[o] = min(255, dr * 4)
                diff[o + 1] = min(255, dg * 4)
                diff[o + 2] = min(255, db * 4)
                diff[o + 3] = 255
    pixels = max(1, (x1 - x0) * (y1 - y0))
    mse = squares / (pixels * 3)
    psnr = math.inf if mse == 0 else 10.0 * math.log10(255.0 * 255.0 / mse)
    if diff is not None:
        write_png(diff_path, width, height, diff)
    return {
        'psnr': 'inf' if psnr == math.inf else f'{psnr:.2f}',
        'mean_abs': f'{total / (pixels * 3):.3f}',
        'max_abs': worst,
        'changed': f'{changed / pixels:.4f}',
        'width': x1 - x0,
        'height': y1 - y0,
        'cropped': int((wa, ha) != (wb, hb)),
    }, psnr


def main():
    parser = argparse.ArgumentParser(description='Compare two frames (PSNR and a diff image), or convert a raw one.')
    parser.add_argument('images', nargs=2)
    parser.add_argument('--diff')
    parser.add_argument('--min-psnr', type=float)
    parser.add_argument('--region')
    parser.add_argument('--size')
    parser.add_argument('--png', action='store_true')
    args = parser.parse_args()
    if args.png:
        width, height, pixels = read_image(args.images[0], args.size)
        write_png(args.images[1], width, height, pixels)
        return 0
    region = tuple(int(v) for v in args.region.split(',')) if args.region else None
    result, psnr = compare(read_image(args.images[0], args.size), read_image(args.images[1], args.size), region, args.diff)
    print(' '.join(f'{k}={v}' for k, v in result.items()))
    if args.min_psnr is not None and psnr < args.min_psnr:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
