#!/usr/bin/env python3
"""Saves a PNG of the mirror window (found by its title).

Keep the window visible and above other windows: covered areas can contain stale
pixels in XWD captures of the test viewer.

Used in spike S3 to see where the AssistiveTouch pointer landed.
Usage: grab_mirror.py OUT.png [--title Porthole] [--max-height 900]
"""

import argparse
import re
import struct
import subprocess

from PIL import Image


def window_id(title):
    out = subprocess.run(["xwininfo", "-name", title], capture_output=True, text=True,
                         check=True).stdout
    return re.search(r"Window id: (0x[0-9a-f]+)", out).group(1)


def grab_xwd(wid):
    """Captures a window with xwd and decodes the XWD (version 7) format."""
    data = subprocess.run(["xwd", "-silent", "-id", wid], capture_output=True, check=True).stdout
    fields = struct.unpack(">25I", data[:100])
    header_size, version = fields[0], fields[1]
    width, height = fields[4], fields[5]
    byte_order, bits_per_pixel, bytes_per_line = fields[7], fields[11], fields[12]
    ncolors = fields[19]
    modes = {(32, 0): "BGRX", (32, 1): "XRGB", (24, 0): "BGR", (24, 1): "RGB"}
    if version != 7 or (bits_per_pixel, byte_order) not in modes:
        raise SystemExit(f"unsupported XWD image (version {version}, {bits_per_pixel} bpp)")
    offset = header_size + ncolors * 12
    raw_mode = modes[(bits_per_pixel, byte_order)]
    return Image.frombuffer("RGB", (width, height), data[offset:offset + bytes_per_line * height],
                            "raw", raw_mode, bytes_per_line, 1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("--title", default="Porthole")
    parser.add_argument("--max-height", type=int, default=900)
    args = parser.parse_args()

    img = grab_xwd(window_id(args.title))
    full = img.size
    if img.height > args.max_height:
        img = img.resize((round(img.width * args.max_height / img.height), args.max_height))
    img.save(args.out)
    print(f"{args.out}: window {full[0]}x{full[1]}, saved {img.width}x{img.height}")


if __name__ == "__main__":
    main()
