#!/usr/bin/env python3
"""Spike S3: tap exact spots on the iPhone using the measured acceleration model plus
closed-loop correction from the mirror video.

Model (measured 2026-11-16 on iOS 26.6.1, AssistiveTouch default tracking speed,
15 ms between reports): during continuous movement, one report of S units moves the
pointer  0.25*S + 0.055*S^2  pixels of a 500-px-wide mirror window.

Small corrective nudges after a pause turned out to be unreliable, so each attempt
starts from the top-left corner and sends the whole move as one continuous burst;
if it misses, the per-axis scale is adjusted from the observed travel and it retries.

Safety: before every tap the screen is compared with a reference image (e.g. the
Calculator keypad). The script stops if the screen doesn't match, the mirror changes
size, or the pointer cannot be positioned within tolerance. Keep the mirror window
visible and above other windows so captures contain current pixels.

Usage:
  tap_test.py LOG_FILE --reference REF.png --region X0,Y0,X1,Y1 X,Y [X,Y ...]
  (all coordinates in mirror-window pixels)
"""

import argparse
import math
import sys
import time
from pathlib import Path

from PIL import Image, ImageChops

sys.path.insert(0, str(Path(__file__).resolve().parent))
from grab_mirror import grab_xwd, window_id  # noqa: E402
from measure import SETTLE, send  # noqa: E402

MODEL_WIDTH = 500.0
STEPS = (32, 16, 8, 4, 2, 1)
TOLERANCE = 5.0          # px: close enough to tap
MAX_ATTEMPTS = 4
ROI = 90                 # px: search radius around where the pointer should be
MAX_DIFF_FRACTION = 0.06  # screen check: share of region pixels allowed to differ


class ScreenMismatch(RuntimeError):
    pass


class PositioningFailed(RuntimeError):
    pass


def model_px(step):
    return 0.25 * step + 0.055 * step * step


def plan_axis(distance, axis, scale):
    """Tokens for the peripheral's `path` command plus the predicted travel."""
    sign = 1 if distance >= 0 else -1
    remaining = abs(distance)
    tokens, predicted = [], 0.0
    for step in STEPS:
        per = model_px(step) * scale
        count = int(remaining // per)
        if count:
            tokens.append(f"{axis}{sign * step}*{count}")
            remaining -= count * per
            predicted += sign * count * per
    return tokens, predicted


def changed_centre(before, after, centre, threshold=24):
    """Centre of what changed between two frames inside a box around `centre`."""
    w, h = before.size
    box = (max(0, int(centre[0] - ROI)), max(0, int(centre[1] - ROI)),
           min(w, int(centre[0] + ROI)), min(h, int(centre[1] + ROI)))
    diff = ImageChops.difference(before.crop(box), after.crop(box)).convert("L")
    bbox = diff.point(lambda v: 255 if v > threshold else 0).getbbox()
    if not bbox:
        return None
    return (box[0] + (bbox[0] + bbox[2]) / 2, box[1] + (bbox[1] + bbox[3]) / 2)


def diff_fraction(a, b, threshold=40):
    diff = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > threshold else 0)
    return sum(1 for v in diff.getdata() if v) / (diff.width * diff.height)


class Tapper:
    def __init__(self, log_file, title, reference, region):
        self.log = log_file
        self.wid = window_id(title)
        self.frame_size = grab_xwd(self.wid).size
        if reference.size != self.frame_size:
            raise ScreenMismatch("mirror size changed; capture a new reference")
        if (len(region) != 4 or not 0 <= region[0] < region[2] <= reference.width
                or not 0 <= region[1] < region[3] <= reference.height):
            raise ValueError("reference region must lie inside the mirror")
        self.reference = reference.crop(region)
        self.region = region
        width = self.frame_size[0]
        self.scale = {"x": width / MODEL_WIDTH, "y": width / MODEL_WIDTH}

    def frame(self):
        frame = grab_xwd(self.wid)
        if frame.size != self.frame_size:
            raise ScreenMismatch("mirror resized during the test; capture a new reference")
        return frame

    def check_screen(self):
        """Raises ScreenMismatch unless the region matches the reference (2 tries)."""
        for _ in range(2):
            frac = diff_fraction(self.reference, self.frame().crop(self.region))
            if frac <= MAX_DIFF_FRACTION:
                return frac
            time.sleep(0.5)
        raise ScreenMismatch(f"screen doesn't match the reference ({frac:.0%} differs)")

    def anchor(self):
        before = self.frame()
        send("anchor", self.log)
        time.sleep(SETTLE)
        pos = changed_centre(before, self.frame(), (20, 20))
        return pos if pos else (16.0, 16.0)  # measured corner position as fallback

    def goto(self, target):
        pos = None
        for attempt in range(1, MAX_ATTEMPTS + 1):
            start = self.anchor()
            tx, px = plan_axis(target[0] - start[0], "x", self.scale["x"])
            ty, py = plan_axis(target[1] - start[1], "y", self.scale["y"])
            before = self.frame()
            send("path " + " ".join(tx + ty), self.log)
            time.sleep(SETTLE)
            pos = changed_centre(before, self.frame(), target)
            if pos is None:
                raise PositioningFailed(f"pointer not found near {target}")
            err = math.hypot(pos[0] - target[0], pos[1] - target[1])
            if err <= TOLERANCE:
                return pos, attempt
            # Learn from this attempt: actual travel vs predicted travel, per axis.
            for axis, predicted, actual in (("x", px, pos[0] - start[0]),
                                            ("y", py, pos[1] - start[1])):
                if abs(predicted) > 20 and actual * predicted > 0:
                    self.scale[axis] *= actual / predicted
        raise PositioningFailed(
            f"pointer is {err:.1f}px from {target} after {MAX_ATTEMPTS} attempts "
            f"(limit {TOLERANCE:.1f}px)")

    def tap(self, target):
        if (not all(math.isfinite(v) for v in target)
                or not 0 <= target[0] < self.frame_size[0]
                or not 0 <= target[1] < self.frame_size[1]):
            raise ValueError(f"target {target} must lie inside the mirror")
        self.check_screen()
        pos, attempts = self.goto(target)
        self.check_screen()
        send("click", self.log)
        err = math.hypot(pos[0] - target[0], pos[1] - target[1])
        print(f"tap {target}: pointer at ({pos[0]:.0f},{pos[1]:.0f}), error {err:.1f}px, "
              f"{attempts} attempt(s), scale x={self.scale['x']:.3f} y={self.scale['y']:.3f}",
              flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log_file")
    parser.add_argument("targets", nargs="+")
    parser.add_argument("--reference", required=True, help="image of the expected screen")
    parser.add_argument("--region", required=True, help="X0,Y0,X1,Y1 to compare")
    parser.add_argument("--title", default="Porthole@spidewol-virtual-machine")
    args = parser.parse_args()

    region = tuple(int(v) for v in args.region.split(","))
    try:
        tapper = Tapper(args.log_file, args.title, Image.open(args.reference).convert("RGB"),
                        region)
        for t in args.targets:
            x, y = (float(v) for v in t.split(","))
            tapper.tap((x, y))
            time.sleep(0.4)
    except (ScreenMismatch, PositioningFailed, ValueError) as e:
        sys.exit(f"STOPPED; no further taps sent: {e}")


if __name__ == "__main__":
    main()
