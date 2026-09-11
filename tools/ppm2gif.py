#!/usr/bin/env python3
"""Turn the preview's .ppms clips into animated GIFs.

A still image cannot show whether motion is smooth, so the animation work is
verified by watching these rather than by reading a description. Needs PIL.
"""
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("needs Pillow: python3 -m pip install pillow")


def read_frames(path):
    """Reads concatenated P6 frames, with an optional leading '# fps N'."""
    data = path.read_bytes()
    pos, fps = 0, 25
    frames = []
    while pos < len(data):
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            end = data.index(b"\n", pos)
            comment = data[pos:end].decode("ascii", "ignore")
            if "fps" in comment:
                fps = int(comment.split()[-1])
            pos = end + 1
            continue
        if data[pos:pos + 2] != b"P6":
            break
        pos += 2
        fields = []
        while len(fields) < 3:
            while data[pos:pos + 1].isspace():
                pos += 1
            start = pos
            while not data[pos:pos + 1].isspace():
                pos += 1
            fields.append(int(data[start:pos]))
        pos += 1
        w, h, _ = fields
        n = w * h * 3
        frames.append(Image.frombytes("RGB", (w, h), data[pos:pos + n]))
        pos += n
    return frames, fps


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "docs/anim")
    clips = sorted(root.glob("*.ppms"))
    if not clips:
        print(f"no .ppms clips in {root}")
        return 1
    total = 0
    for src in clips:
        frames, fps = read_frames(src)
        if not frames:
            print(f"{src}: no frames")
            continue
        # One palette for the whole clip, built from every frame stacked into
        # a single image. A per-frame palette makes the colours crawl.
        w, h = frames[0].size
        montage = Image.new("RGB", (w, h * len(frames)))
        for i, f in enumerate(frames):
            montage.paste(f, (0, i * h))
        master = montage.quantize(colors=128, method=Image.FASTOCTREE)
        # Dither off: ordered dithering on an LED glow looks like noise, and it
        # would hide the temporal dithering these clips exist to show.
        quant = [f.quantize(palette=master, dither=Image.Dither.NONE) for f in frames]
        dst = src.with_suffix(".gif")
        quant[0].save(dst, save_all=True, append_images=quant[1:],
                      duration=int(round(1000 / fps)), loop=0, disposal=1)
        src.unlink()
        kb = dst.stat().st_size // 1024
        total += kb
        print(f"{dst}  {w}x{h}  {len(frames)} frames  {kb} KB")
    print(f"total {total} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
