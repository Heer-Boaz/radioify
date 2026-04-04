#!/usr/bin/env python3
"""Generate radioify.ico with all standard Windows icon sizes from the 256x256 source."""

import argparse
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Pillow is required: pip install Pillow", file=sys.stderr)
    sys.exit(1)

SIZES = [16, 24, 32, 48, 64, 256]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "source",
        nargs="?",
        default=None,
        help="Source PNG or ICO (default: radioify.ico in repo root)",
    )
    parser.add_argument(
        "-o", "--output",
        default=None,
        help="Output ICO path (default: overwrites source)",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    source = Path(args.source) if args.source else repo_root / "radioify.ico"
    output = Path(args.output) if args.output else source

    img = Image.open(source).convert("RGBA")
    print(f"Source: {source} ({img.size[0]}x{img.size[1]})")

    img.save(output, format="ICO", sizes=[(s, s) for s in SIZES])

    with open(output, "rb") as f:
        count = struct.unpack("<H", f.read(6)[4:6])[0]
        print(f"Output: {output} ({count} images)")
        for _ in range(count):
            entry = f.read(16)
            w = entry[0] or 256
            h = entry[1] or 256
            bpp = struct.unpack("<H", entry[6:8])[0]
            size = struct.unpack("<I", entry[8:12])[0]
            print(f"  {w}x{h} {bpp}bpp ({size} bytes)")

    print(f"Total: {output.stat().st_size} bytes")


if __name__ == "__main__":
    main()
