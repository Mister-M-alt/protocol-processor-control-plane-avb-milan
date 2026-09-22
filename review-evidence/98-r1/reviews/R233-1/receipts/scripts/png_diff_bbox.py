#!/usr/bin/env python3
"""R233 probe: decode two 8-bit RGB/RGBA non-interlaced PNGs (stdlib only) and report
the bounding box and count of differing pixels."""
import sys, zlib, struct

def decode(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, ihdr = 8, b'', None
    while pos < len(data):
        ln, typ = struct.unpack('>I4s', data[pos:pos+8])
        body = data[pos+8:pos+8+ln]
        if typ == b'IHDR': ihdr = struct.unpack('>IIBBBBB', body)
        elif typ == b'IDAT': idat += body
        pos += 12 + ln
    w, h, depth, ctype, _, _, inter = ihdr
    assert depth == 8 and inter == 0 and ctype in (2, 6), ihdr
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[i]; line = bytearray(raw[i+1:i+1+stride]); i += 1 + stride
        for x in range(stride):
            a = line[x-bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x-bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line)); prev = line
    return w, h, bpp, rows

w1, h1, b1, r1 = decode(sys.argv[1]); w2, h2, b2, r2 = decode(sys.argv[2])
print(f'{sys.argv[1]}: {w1}x{h1} bpp={b1}'); print(f'{sys.argv[2]}: {w2}x{h2} bpp={b2}')
if (w1, h1, b1) != (w2, h2, b2): print('DIMENSIONS DIFFER'); sys.exit(0)
minx = miny = 10**9; maxx = maxy = -1; n = 0
for y in range(h1):
    if r1[y] == r2[y]: continue
    for x in range(w1):
        if r1[y][x*b1:(x+1)*b1] != r2[y][x*b1:(x+1)*b1]:
            n += 1; minx = min(minx, x); maxx = max(maxx, x); miny = min(miny, y); maxy = max(maxy, y)
print(f'differing pixels: {n}')
if n: print(f'bounding box (px, scale 2 render): x {minx}..{maxx}, y {miny}..{maxy}')
