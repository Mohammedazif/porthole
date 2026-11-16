#!/usr/bin/env python3
"""Spike S3: measure how far the iOS pointer moves for a given HID movement.

Drives ble_hid_peripheral.py through its FIFO and finds the AssistiveTouch pointer in
the mirror window by comparing each frame with a clean frame taken while the pointer
is auto-hidden.

Usage:
  measure.py LOG_FILE [--axis x|y] [--grid 1x200,2x100,...] [--save DIR]

Each grid entry STEPxREPORTS sends REPORTS movement reports of STEP units each,
starting from the top-left corner. LOG_FILE is the peripheral's output (used to know
when a command has finished). Keep the iPhone on a static screen while it runs.
"""

import argparse
import os
import sys
import time
from pathlib import Path

from PIL import ImageChops

sys.path.insert(0, str(Path(__file__).resolve().parent))
from grab_mirror import grab_xwd, window_id  # noqa: E402

FIFO = f"{os.environ.get('XDG_RUNTIME_DIR', '/tmp')}/porthole-hid.fifo"
SETTLE = 0.7       # seconds: mirror latency + pointer animation
HIDE_WAIT = 6.0    # seconds without movement before iOS hides the pointer


def send(cmd, log_file, timeout=60.0):
    """Sends a FIFO command and waits for the peripheral to log 'done: CMD'."""
    marker = f"done: {cmd}"
    before = Path(log_file).read_text().count(marker)
    with open(FIFO, "w") as fifo:
        fifo.write(cmd + "\n")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if Path(log_file).read_text().count(marker) > before:
            return
        time.sleep(0.05)
    raise TimeoutError(cmd)


def find_pointer(clean, frame, threshold=24):
    """Returns (centre, bbox) of the pointer: the region that differs from `clean`."""
    mask = ImageChops.difference(clean, frame).convert("L").point(
        lambda v: 255 if v > threshold else 0)
    box = mask.getbbox()
    if not box:
        return None, None
    return ((box[0] + box[2]) / 2, (box[1] + box[3]) / 2), box


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log_file")
    parser.add_argument("--title", default="Porthole@spidewol-virtual-machine")
    parser.add_argument("--axis", choices=["x", "y"], default="x")
    parser.add_argument("--grid", default="1x200,2x100,4x50,8x25,16x12,32x6,64x3")
    parser.add_argument("--save", help="directory to save frames")
    args = parser.parse_args()

    wid = window_id(args.title)
    print(f"waiting {HIDE_WAIT:.0f}s for the pointer to hide...", flush=True)
    time.sleep(HIDE_WAIT)
    clean = grab_xwd(wid)
    w, h = clean.size
    if args.save:
        clean.save(f"{args.save}/clean.png")
    span = w if args.axis == "x" else h

    print(f"mirror window {w}x{h}; axis {args.axis}")
    print(f"{'step':>5} {'reports':>7} {'units':>6} {'pos px':>7} {'px/report':>10} "
          f"{'px/unit':>8} {'% screen':>9}  bbox")
    for entry in args.grid.split(","):
        step, reports = (int(v) for v in entry.split("x"))
        send("anchor", args.log_file)
        total = step * reports  # split by the peripheral into `reports` equal reports
        dx, dy = (total, 0) if args.axis == "x" else (0, total)
        send(f"move {dx} {dy} {step}", args.log_file)
        time.sleep(SETTLE)
        frame = grab_xwd(wid)
        if args.save:
            frame.save(f"{args.save}/{args.axis}_{step}x{reports}.png")
        centre, box = find_pointer(clean, frame)
        if not centre:
            print(f"{step:>5} {reports:>7} {step * reports:>6}   pointer not found")
            continue
        pos = centre[0] if args.axis == "x" else centre[1]
        print(f"{step:>5} {reports:>7} {step * reports:>6} {pos:>7.0f} {pos / reports:>10.2f} "
              f"{pos / (step * reports):>8.2f} {100 * pos / span:>8.1f}%  {box}", flush=True)


if __name__ == "__main__":
    main()
