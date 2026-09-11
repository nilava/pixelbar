#!/usr/bin/env python3
"""Convert the preview renderer's PPM output to PNG. Standard library only."""
import struct
import sys
import zlib
from pathlib import Path


def read_ppm(path):
    data = path.read_bytes()
    # Header: P6 <w> <h> <maxval>, whitespace separated, '#' comments allowed.
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while data[pos : pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1  # single whitespace byte after the maxval
    w, h, _maxval = fields
    return w, h, data[pos : pos + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3 : (y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    ppms = sorted(root.glob("*.ppm"))
    if not ppms:
        print(f"no .ppm files in {root}")
        return 1
    for src in ppms:
        w, h, rgb = read_ppm(src)
        dst = src.with_suffix(".png")
        write_png(dst, w, h, rgb)
        src.unlink()
        print(f"{dst}  {w}x{h}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
