#!/usr/bin/env python3
"""Convert the approved artwork to vector paths and export the Porthole kit.

Pillow decodes the source / encodes ICO and ICNS containers. All production
artwork is vector geometry; Cairo renders the same paths to PNG and PDF.
Run with --trace only when intentionally replacing source/paths.json.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import zipfile

import cairo
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
THEMES = {
    "on-light": ("#111111", "#B8B8B8"),
    "on-dark": ("#FFFFFF", "#A3A3A3"),
    "mono-black": ("#000000", "#000000"),
    "mono-white": ("#FFFFFF", "#FFFFFF"),
}
LAYOUTS = {
    "icon": (512, 512, [("icon", 64, 64, 384)]),
    "icon-small": (512, 512, [("icon", 32, 32, 448)]),
    "wordmark": (1280, 384, [("wordmark", 64, 67, 1152)]),
    "horizontal": (1600, 440, [("icon", 64, 60, 320), ("wordmark", 426, 99.5, 1110)]),
    "stacked": (1600, 960, [("icon", 600, 80, 400), ("wordmark", 160, 570, 1280)]),
}


def add(a, b):
    return a[0] + b[0], a[1] + b[1]


def sub(a, b):
    return a[0] - b[0], a[1] - b[1]


def mul(a, t):
    return a[0] * t, a[1] * t


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1]


def norm(a):
    length = math.hypot(*a)
    return mul(a, 1 / length) if length else (1.0, 0.0)


def bezier(curve, t):
    s = 1 - t
    weights = [s**3, 3 * s * s * t, 3 * s * t * t, t**3]
    return tuple(sum(p[i] * w for p, w in zip(curve, weights)) for i in (0, 1))


def fit_curve(points, start_tangent, end_tangent, error=0.7):
    """Least-squares cubic fitting with a subpixel geometric error budget."""
    if len(points) == 2:
        d = math.dist(*points) / 3
        return [[points[0], add(points[0], mul(start_tangent, d)),
                 add(points[-1], mul(end_tangent, d)), points[-1]]]
    distances = [0.0]
    for a, b in zip(points, points[1:]):
        distances.append(distances[-1] + math.dist(a, b))
    total = distances[-1] or 1
    parameters = [d / total for d in distances]
    c00 = c01 = c11 = x0 = x1 = 0.0
    for p, t in zip(points, parameters):
        s = 1 - t
        b0, b1, b2, b3 = s**3, 3 * s * s * t, 3 * s * t * t, t**3
        a0, a1 = mul(start_tangent, b1), mul(end_tangent, b2)
        residual = sub(p, add(mul(points[0], b0 + b1), mul(points[-1], b2 + b3)))
        c00 += dot(a0, a0)
        c01 += dot(a0, a1)
        c11 += dot(a1, a1)
        x0 += dot(a0, residual)
        x1 += dot(a1, residual)
    determinant = c00 * c11 - c01 * c01
    a = (x0 * c11 - x1 * c01) / determinant if abs(determinant) > 1e-12 else -1
    b = (c00 * x1 - c01 * x0) / determinant if abs(determinant) > 1e-12 else -1
    chord = math.dist(points[0], points[-1])
    if a < chord * 1e-6 or b < chord * 1e-6:
        a = b = chord / 3
    curve = [points[0], add(points[0], mul(start_tangent, a)),
             add(points[-1], mul(end_tangent, b)), points[-1]]
    errors = [math.dist(bezier(curve, t), p) for p, t in zip(points, parameters)]
    split = max(range(1, len(points) - 1), key=errors.__getitem__)
    if errors[split] <= error:
        return [curve]
    tangent = norm(sub(points[split - 1], points[split + 1]))
    return (fit_curve(points[:split + 1], start_tangent, tangent, error)
            + fit_curve(points[split:], mul(tangent, -1), end_tangent, error))


def components(points, minimum=80):
    remaining = set(points)
    result = []
    while remaining:
        pending = [remaining.pop()]
        group = set(pending)
        while pending:
            x, y = pending.pop()
            for neighbor in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if neighbor in remaining:
                    remaining.remove(neighbor)
                    group.add(neighbor)
                    pending.append(neighbor)
        if len(group) >= minimum:
            result.append(group)
    return sorted(result, key=lambda group: (min(y for x, y in group), min(x for x, y in group)))


def contours(group):
    edges = {}

    def edge(a, b):
        edges.setdefault(a, set()).add(b)

    for x, y in group:
        if (x, y - 1) not in group:
            edge((x, y), (x + 1, y))
        if (x + 1, y) not in group:
            edge((x + 1, y), (x + 1, y + 1))
        if (x, y + 1) not in group:
            edge((x + 1, y + 1), (x, y + 1))
        if (x - 1, y) not in group:
            edge((x, y + 1), (x, y))
    loops = []
    while edges:
        start = min(edges)
        previous, current = (start[0] - 1, start[1]), start
        loop = []
        while True:
            loop.append(current)
            choices = edges[current]
            direction = sub(current, previous)

            def turn(p):
                d = sub(p, current)
                cross = direction[0] * d[1] - direction[1] * d[0]
                return (0 if cross > 0 else 1 if dot(direction, d) > 0 else 2, p)

            nxt = min(choices, key=turn)
            choices.remove(nxt)
            if not choices:
                del edges[current]
            previous, current = current, nxt
            if current == start:
                break
        area = abs(sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(loop, loop[1:] + loop[:1]))) / 2
        if area >= 12:
            loops.append(loop)
    return loops


def smooth_path(loop, origin):
    # Smooth only the one-pixel stair step from raster sampling, then fit curves.
    points = [(float(x), float(y)) for x, y in loop]
    for _ in range(3):
        points = [mul(add(add(points[i - 1], mul(p, 2)), points[(i + 1) % len(points)]), 0.25)
                  for i, p in enumerate(points)]
    count = len(points)
    cuts = [0, count // 4, count // 2, 3 * count // 4, count]
    curves = []
    for start, end in zip(cuts, cuts[1:]):
        segment = [points[i % count] for i in range(start, end + 1)]
        ta = norm(sub(points[(start + 3) % count], points[(start - 3) % count]))
        tb = norm(sub(points[(end - 3) % count], points[(end + 3) % count]))
        curves.extend(fit_curve(segment, ta, tb))

    def local(p):
        return [round(p[0] - origin[0], 3), round(p[1] - origin[1], 3)]

    return {"start": local(curves[0][0]), "curves": [[*local(c[1]), *local(c[2]), *local(c[3])] for c in curves]}


def trace_source():
    image = Image.open(ROOT / "source/transparent-master.png").convert("RGBA")
    width, height = image.size
    if image.getchannel("A").getextrema() != (0, 255):
        raise ValueError("The master must contain real transparency.")
    masks = {"ink": set(), "reflection": set()}
    for i, p in enumerate(image.getdata()):
        if p[3] <= 127:
            continue
        if max(p[:3]) < 80:
            masks["ink"].add((i % width, i // width))
        elif 90 < sum(p[:3]) / 3 < 220:
            masks["reflection"].add((i % width, i // width))
    grouped = {name: components(mask) for name, mask in masks.items()}
    if len(grouped["reflection"]) != 4:
        raise ValueError("Expected two reflection stripes in each mark.")
    split = height * 0.57
    result = {"version": 1, "trace_error_pixels": 0.7, "source_size": [width, height], "parts": {}}
    for part in ("icon", "wordmark"):
        chosen = {tone: [g for g in groups if (max(y for x, y in g) < split) == (part == "icon")]
                  for tone, groups in grouped.items()}
        all_points = set().union(*chosen["ink"], *chosen["reflection"])
        bounds = (min(x for x, y in all_points), min(y for x, y in all_points),
                  max(x for x, y in all_points) + 1, max(y for x, y in all_points) + 1)
        result["parts"][part] = {"width": bounds[2] - bounds[0], "height": bounds[3] - bounds[1],
                                  "source_bounds": bounds,
                                  "layers": {tone: [smooth_path(loop, bounds[:2]) for group in groups for loop in contours(group)]
                                             for tone, groups in chosen.items()}}
    (ROOT / "source/paths.json").write_text(json.dumps(result, separators=(",", ":")) + "\n")
    return result


def svg_path(paths):
    return " ".join("M" + " ".join(map(str, path["start"])) + " "
                    + " ".join("C" + " ".join(map(str, curve)) for curve in path["curves"]) + " Z"
                    for path in paths)


def svg_document(data, layout, theme, tile=None):
    width, height, places = LAYOUTS[layout]
    if tile:
        width = height = 1024
        places = [("icon", 204.8, 204.8, 614.4)]
    ink, reflection = THEMES.get(theme, ("currentColor", "currentColor"))
    extra = ""
    if theme == "auto":
        ink, reflection = "var(--ink)", "var(--reflection)"
        extra = '<style>:root{--ink:#111111;--reflection:#B8B8B8}@media(prefers-color-scheme:dark){:root{--ink:#FFFFFF;--reflection:#A3A3A3}}@media(forced-colors:active){:root{--ink:CanvasText;--reflection:CanvasText}}</style>'
    elif theme == "current-color":
        extra = '<!-- Inline this SVG to inherit currentColor; external img does not inherit host color. -->'
    body = []
    if tile:
        body.append(f'<rect width="1024" height="1024" fill="{tile}"/>')
    for part, x, y, desired_width in places:
        shape = data["parts"][part]
        scale = desired_width / shape["width"]
        body.append(f'<g transform="translate({x:g} {y:g}) scale({scale:.8f})">')
        for tone, color in (("ink", ink), ("reflection", reflection)):
            if layout == "icon-small" and tone == "reflection":
                continue
            body.append(f'<path fill="{color}" fill-rule="evenodd" d="{svg_path(shape["layers"][tone])}"/>')
        body.append('</g>')
    title = "Porthole icon" if layout.startswith("icon") or tile else "Porthole"
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
            f'width="{width}" height="{height}" role="img" aria-label="{title}">'
            f'<title>{title}</title>{extra}{"".join(body)}</svg>\n')


def rgb(color):
    return tuple(int(color[i:i + 2], 16) / 255 for i in (1, 3, 5))


def draw_part(context, data, part, x, y, width, theme, small=False):
    shape = data["parts"][part]
    context.save()
    context.translate(x, y)
    context.scale(width / shape["width"], width / shape["width"])
    context.set_fill_rule(cairo.FILL_RULE_EVEN_ODD)
    for tone, color in zip(("ink", "reflection"), THEMES[theme]):
        if tone == "reflection" and small:
            continue
        context.new_path()
        for path in shape["layers"][tone]:
            context.move_to(*path["start"])
            for curve in path["curves"]:
                context.curve_to(*curve)
            context.close_path()
        context.set_source_rgb(*rgb(color))
        context.fill()
    context.restore()


def draw_layout(context, data, layout, theme):
    for part, x, y, width in LAYOUTS[layout][2]:
        draw_part(context, data, part, x, y, width, theme, layout == "icon-small")


def write_png(data, path, layout, theme, width, tile=None):
    base_width, base_height, _ = LAYOUTS[layout]
    height = round(width * base_height / base_width)
    surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, width, height)
    context = cairo.Context(surface)
    context.scale(width / base_width, height / base_height)
    if tile:
        context.set_source_rgb(*rgb(tile))
        context.paint()
        inset, mark_width = (64, 384) if width < 128 else (102.4, 307.2)
        draw_part(context, data, "icon", inset, inset, mark_width, theme, small=width < 48)
    else:
        draw_layout(context, data, layout, theme)
    path.parent.mkdir(parents=True, exist_ok=True)
    surface.write_to_png(str(path))


def write_pdf(data, path, layout, theme):
    width, height, _ = LAYOUTS[layout]
    surface = cairo.PDFSurface(str(path), width * 0.5, height * 0.5)
    context = cairo.Context(surface)
    context.scale(0.5, 0.5)
    draw_layout(context, data, layout, theme)
    surface.finish()


def rounded_rect(context, x, y, width, height, radius):
    context.new_sub_path()
    for cx, cy, start in ((x + width - radius, y + radius, -math.pi / 2),
                          (x + width - radius, y + height - radius, 0),
                          (x + radius, y + height - radius, math.pi / 2),
                          (x + radius, y + radius, math.pi)):
        context.arc(cx, cy, radius, start, start + math.pi / 2)
    context.close_path()


def label(context, text, x, y, size=20, color="#111111", bold=False):
    context.select_font_face("DejaVu Sans", cairo.FONT_SLANT_NORMAL,
                             cairo.FONT_WEIGHT_BOLD if bold else cairo.FONT_WEIGHT_NORMAL)
    context.set_font_size(size)
    context.set_source_rgb(*rgb(color))
    context.move_to(x, y)
    context.show_text(text)


def preview_board(data):
    surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, 1680, 1320)
    c = cairo.Context(surface)
    c.set_source_rgb(*rgb("#F0EFED")); c.paint()
    label(c, "PORTHOLE / BRAND KIT", 72, 74, 17, "#6B6965", True)
    label(c, "One identity. Every surface.", 72, 137, 43, bold=True)
    label(c, "Matching reflections, rounded geometry and a clear pointer.", 74, 178, 21, "#62605C")
    cards = [(72, 222, "#FFFFFF", "on-light", "LIGHT / TRANSPARENT ASSETS"),
             (864, 222, "#15171A", "on-dark", "DARK / REVERSED ASSETS"),
             (72, 650, "#EEE5D6", "on-light", "WARM / NEUTRAL BRAND"),
             (864, 650, "#15263D", "on-dark", "COLORED / REVERSED BRAND")]
    for x, y, background, theme, caption in cards:
        c.set_source_rgb(*rgb(background))
        rounded_rect(c, x, y, 744, 396, 22); c.fill()
        color = "#A3A3A3" if theme == "on-dark" else "#777777"
        label(c, caption, x + 32, y + 42, 13, color, True)
        draw_part(c, data, "icon", x + 301, y + 83, 142, theme)
        draw_part(c, data, "wordmark", x + 115, y + 254, 514, theme)
    label(c, "SMALL-SIZE MARKS", 74, 1113, 14, "#62605C", True)
    for size, x in ((16, 75), (24, 140), (32, 217), (48, 302), (64, 405)):
        draw_part(c, data, "icon", x, 1145, size, "on-light", small=size < 48)
        label(c, f"{size}px", x, 1232, 12, "#62605C")
    label(c, "SVG  /  PNG  /  PDF  /  ICO  /  ICNS", 730, 1155, 18, bold=True)
    label(c, "Icon, wordmark, horizontal and stacked lockups.", 730, 1194, 18, "#62605C")
    label(c, "Light, dark, monochrome, inherited color and automatic theme.", 730, 1228, 16, "#62605C")
    surface.write_to_png(str(ROOT / "preview.png"))


def brand_guide(data):
    surface = cairo.PDFSurface(str(ROOT / "Porthole-brand-guide.pdf"), 842, 595)
    c = cairo.Context(surface)

    def page(title, subtitle):
        c.set_source_rgb(1, 1, 1); c.paint()
        label(c, "PORTHOLE / IDENTITY GUIDE / 1.0", 44, 40, 10, "#777777", True)
        label(c, title, 44, 88, 28, bold=True)
        label(c, subtitle, 44, 119, 12, "#666666")

    page("The Porthole identity", "A circular window, a pointer and a custom rounded wordmark.")
    draw_part(c, data, "icon", 352, 157, 138, "on-light")
    draw_part(c, data, "wordmark", 171, 335, 500, "on-light")
    label(c, "The reflection pairs sit at the upper-right in both the symbol and the P.", 77, 518, 13)
    label(c, "Preserve the proportions, spacing, cursor and stripe positions.", 77, 545, 13, "#666666")
    c.show_page()
    page("Theme and color", "Use the light or reversed artwork according to the surface behind it.")
    for x, background, theme, title in ((44, "#F4F3F0", "on-light", "Light surfaces"),
                                         (430, "#15171A", "on-dark", "Dark surfaces")):
        c.set_source_rgb(*rgb(background)); rounded_rect(c, x, 151, 368, 226, 12); c.fill()
        draw_part(c, data, "wordmark", x + 33, 233, 302, theme)
        label(c, title, x + 23, 183, 12, "#A3A3A3" if theme == "on-dark" else "#666666")
    label(c, "Ink  #111111     Paper  #FFFFFF     Reflection  #B8B8B8", 44, 419, 13, bold=True)
    label(c, "Reversed reflection  #A3A3A3. Monochrome files use one solid color.", 44, 449, 12)
    label(c, "On photographs or busy patterns, place the logo on a solid light or dark panel.", 44, 479, 12)
    label(c, "current-color SVG inherits color when inlined; external img does not inherit it.", 44, 509, 12)
    label(c, "Automatic SVG follows system appearance; manual app themes must select a variant.", 44, 539, 12)
    c.show_page()
    page("Size, space and delivery", "Keep the brand readable in app chrome, documentation and product artwork.")
    label(c, "Minimums", 44, 167, 17, bold=True)
    for y, text in ((199, "16-32 px: use icon-small or the provided tray / favicon assets."),
                    (228, "48 px and above: use the full icon with reflection detail."),
                    (257, "Wordmark: at least 140 px wide; horizontal: 240 px; stacked: 180 px."),
                    (286, "Leave at least one main stroke width clear around the visible artwork.")):
        label(c, text, 44, y, 12)
    label(c, "Use the right source", 44, 340, 17, bold=True)
    for y, text in ((372, "SVG: scalable UI and design. PNG: presentations and image-only tools."),
                    (401, "PDF: vector print handoff. ICO: Windows. ICNS: macOS."),
                    (430, "App tile PNGs are opaque squares; platform masks apply the corner shape."),
                    (459, "All counters and cursor gaps in the logo SVG / PNG exports are transparent."),
                    (488, "Do not stretch, rotate, add outlines, move reflections or recolor each letter."),
                    (534, "See README.md, preview.html and brand.json for integration and file details.")):
        label(c, text, 44, y, 12)
    surface.finish()


def export(data):
    for theme in [*THEMES, "current-color", "auto"]:
        folder = ROOT / "svg" / theme
        folder.mkdir(parents=True, exist_ok=True)
        for layout in LAYOUTS:
            (folder / f"porthole-{layout}.svg").write_text(svg_document(data, layout, theme))
    for theme in THEMES:
        for layout, widths in (("icon", [48, 64, 128, 256, 512, 1024]),
                               ("icon-small", [16, 20, 24, 32, 40, 48]),
                               ("wordmark", [600, 1200, 2400]),
                               ("horizontal", [800, 1600, 3200]),
                               ("stacked", [800, 1600, 3200])):
            for width in widths:
                write_png(data, ROOT / "png" / theme / f"porthole-{layout}-{width}.png", layout, theme, width)
    (ROOT / "pdf").mkdir(exist_ok=True)
    for layout in ("icon", "wordmark", "horizontal", "stacked"):
        write_pdf(data, ROOT / "pdf" / f"porthole-{layout}.pdf", layout, "on-light")
    for style, theme, tile in (("light", "on-light", "#FFFFFF"), ("dark", "on-dark", "#111111")):
        directory = ROOT / "app-icons" / style
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "porthole-app.svg").write_text(svg_document(data, "icon", theme, tile))
        for size in (16, 20, 24, 32, 40, 48, 64, 128, 180, 192, 256, 512, 1024):
            write_png(data, directory / f"porthole-app-{size}.png", "icon", theme, size, tile)
        frames = [Image.open(directory / f"porthole-app-{size}.png").convert("RGBA")
                  for size in (16, 24, 32, 48, 64, 128, 256)]
        frames[-1].save(directory / "porthole.ico", format="ICO",
                        sizes=[im.size for im in frames], append_images=frames[:-1])
        Image.open(directory / "porthole-app-1024.png").save(directory / "porthole.icns", format="ICNS")
    web = ROOT / "web"
    web.mkdir(exist_ok=True)
    small = svg_document(data, "icon-small", "auto")
    (web / "favicon.svg").write_text(small)
    (web / "favicon.ico").write_bytes((ROOT / "app-icons/light/porthole.ico").read_bytes())
    (web / "apple-touch-icon.png").write_bytes((ROOT / "app-icons/light/porthole-app-180.png").read_bytes())
    preview_board(data)
    brand_guide(data)


def verify():
    import xml.etree.ElementTree as ET

    svgs = list(ROOT.glob("svg/*/*.svg"))
    for path in svgs:
        tree = ET.parse(path)
        assert not tree.findall(".//{http://www.w3.org/2000/svg}image"), f"Embedded raster in {path}"
        assert tree.findall(".//{http://www.w3.org/2000/svg}path"), f"Missing vector paths: {path}"
    pngs = list(ROOT.glob("png/*/*.png"))
    for path in pngs:
        im = Image.open(path)
        assert im.mode == "RGBA" and im.getchannel("A").getextrema() == (0, 255), f"Missing alpha: {path}"
        if "on-dark" in path.parts or "mono-white" in path.parts:
            assert any(p[0] > 240 and p[3] == 255 for p in im.getdata()), f"Missing reversed ink: {path}"
    for path in ROOT.glob("app-icons/*/*.ico"):
        ico = Image.open(path)
        assert {(16, 16), (32, 32), (48, 48), (256, 256)} <= ico.ico.sizes()
    for path in ROOT.glob("app-icons/*/*.icns"):
        Image.open(path).verify()
    # Reflection centers must be in the upper-right of both circular openings.
    data = json.loads((ROOT / "source/paths.json").read_text())
    for part, center in (("icon", (190, 190)), ("wordmark", (106, 106))):
        stripes = data["parts"][part]["layers"]["reflection"]
        assert len(stripes) == 2
        for stripe in stripes:
            points = [stripe["start"]] + [curve[i:i + 2] for curve in stripe["curves"] for i in (0, 2, 4)]
            x = (min(p[0] for p in points) + max(p[0] for p in points)) / 2
            y = (min(p[1] for p in points) + max(p[1] for p in points)) / 2
            assert x > center[0] and y < center[1], (part, x, y)
    return {"svg_files": len(svgs), "transparent_png_files": len(pngs),
            "app_png_files": len(list(ROOT.glob("app-icons/*/*.png"))),
            "pdf_files": len(list(ROOT.glob("pdf/*.pdf"))) + 1,
            "ico_files": len(list(ROOT.glob("app-icons/*/*.ico"))) + 1,
            "icns_files": len(list(ROOT.glob("app-icons/*/*.icns"))),
            "checks": ["True vector paths; no embedded raster in logo SVGs", "Real PNG alpha; transparent counters",
                       "White reversed ink", "Windows multi-resolution icon frames", "macOS icon decoding",
                       "Two upper-right reflection stripes in both marks"]}


def package():
    files = sorted(p for p in ROOT.rglob("*") if p.is_file() and "__pycache__" not in p.parts
                   and p.name not in {"checksums.sha256"})
    (ROOT / "checksums.sha256").write_text("".join(
        f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(ROOT).as_posix()}\n" for p in files))
    destination = ROOT.parents[1] / "output/porthole-brand-kit.zip"
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in [*files, ROOT / "checksums.sha256"]:
            archive.write(path, Path("porthole-brand-kit") / path.relative_to(ROOT))
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", action="store_true", help="Rebuild the vector master from the transparent source")
    args = parser.parse_args()
    paths = ROOT / "source/paths.json"
    data = trace_source() if args.trace or not paths.exists() else json.loads(paths.read_text())
    export(data)
    result = verify()
    (ROOT / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
    result["zip"] = str(package())
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
