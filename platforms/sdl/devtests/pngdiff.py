#!/usr/bin/env python3
"""Pixel diff of two PNGs as the app writes them (8-bit RGB or RGBA, not interlaced). Standard library only.

    pngdiff.py A.png B.png [DIFF.png]

Prints "same", or the number of differing pixels and their bounding box; exit 0 when the images
match, 1 when they differ, 2 on a size mismatch or unreadable file. DIFF.png (optional) is A dimmed
with every differing pixel in magenta.
"""
import struct
import sys
import zlib


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError(f"{path}: only 8-bit RGB / RGBA, not interlaced")
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
        pos += n + 12
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        ftype = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ftype == 1:
                line[x] = (line[x] + a) & 255
            elif ftype == 2:
                line[x] = (line[x] + b) & 255
            elif ftype == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    # RGB triples per row
    return w, h, [[row[i * bpp:i * bpp + 3] for i in range(w)] for row in rows]


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + b"".join(r) for r in rows)
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__.strip())
        return 2
    try:
        wa, ha, a = read_png(sys.argv[1])
        wb, hb, b = read_png(sys.argv[2])
    except (OSError, ValueError, zlib.error) as e:
        print(e)
        return 2
    if (wa, ha) != (wb, hb):
        print(f"size differs: {wa}x{ha} vs {wb}x{hb}")
        return 2
    diff, box = 0, [wa, ha, -1, -1]
    out = []
    for y in range(ha):
        line = []
        for x in range(wa):
            if a[y][x] != b[y][x]:
                diff += 1
                box = [min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)]
                line.append(b"\xff\x00\xff")
            else:
                line.append(bytes(v // 3 for v in a[y][x]))
        out.append(line)
    if diff == 0:
        print("same")
        return 0
    print(f"{diff} pixels differ, box x {box[0]}..{box[2]} y {box[1]}..{box[3]}")
    if len(sys.argv) == 4:
        write_png(sys.argv[3], wa, ha, out)
    return 1


if __name__ == "__main__":
    sys.exit(main())
