# Porthole brand kit

The approved Porthole identity, prepared for light, dark, colored and high-contrast surfaces. The porthole icon and the custom P share two reflection stripes at the upper-right. All logo lettering is outlined; no font installation is needed.

Open [preview.html](preview.html) for the interactive theme and layout preview, [preview.png](preview.png) for the overview, or [Porthole-brand-guide.pdf](Porthole-brand-guide.pdf) for the printable guide.

## Choose an asset

| Need | File or folder |
| --- | --- |
| Header or navigation | `svg/on-light/porthole-horizontal.svg` |
| Dark header | `svg/on-dark/porthole-horizontal.svg` |
| Standalone name | `svg/{theme}/porthole-wordmark.svg` |
| App symbol | `svg/{theme}/porthole-icon.svg` |
| Centered cover or splash artwork | `svg/{theme}/porthole-stacked.svg` |
| 16–32 px controls and trays | `svg/{theme}/porthole-icon-small.svg` or matching PNG |
| Inherit a custom theme color | `svg/current-color/` inlined into markup, or the CSS mask helper |
| Follow operating-system appearance | `svg/auto/` |
| Presentations and image-only tools | `png/` |
| Vector print handoff | `pdf/` |
| Windows application icon | `app-icons/light/porthole.ico` or `app-icons/dark/porthole.ico` |
| macOS icon container | `app-icons/{light,dark}/porthole.icns` |
| Linux / cross-platform application icons | `app-icons/{light,dark}/porthole-app-{size}.png` |
| Browser favicon | `web/favicon.svg`, with `web/favicon.ico` as fallback |
| Apple touch icon | `web/apple-touch-icon.png` |
| Web app manifest | `web/site.webmanifest` |

`{theme}` means one of the following folders:

| Theme | Artwork | Use |
| --- | --- | --- |
| `on-light` | Ink `#111111`, reflections `#B8B8B8` | White and light surfaces |
| `on-dark` | White `#FFFFFF`, reflections `#A3A3A3` | Black and dark surfaces |
| `mono-black` | Solid black, including reflections | One-color production and light high-contrast surfaces |
| `mono-white` | Solid white, including reflections | One-color reversed production and dark high-contrast surfaces |
| `current-color` | All paths use `currentColor` | Inline SVG and themeable UI masks |
| `auto` | Light/dark colors selected by an SVG media query | System appearance and browser favicons |

Warm, navy and other colors in the preview are example backgrounds, not additional brand colors. For arbitrary themes, choose the dark or light artwork according to the background. On photos, gradients or busy textures, use a solid backing panel. No single transparent colorway is legible on every possible background.

## Transparent and scalable

All files in `svg/` contain true vector paths, including the custom lettering. There are no embedded bitmaps or font dependencies. SVGs have no background rectangle. Their windows, letter counters and cursor gap are transparent. PNGs in `png/` have real alpha transparency and are rendered from those same paths.

The app tiles in `app-icons/` deliberately have opaque light or dark square backgrounds. The 180, 192, 512 and 1024 px tile exports use a centered symbol within 60% of the canvas, suitable for platform masking. Small desktop tiles use a larger symbol for readability. Leave the square corners intact when handing a tile to a platform that supplies its own mask.

The `icon-small` version omits the reflection stripes and uses more of its canvas. Use it at 16–32 px. The full icon preserves both stripes and is intended for 48 px and above. The small app PNGs omit the stripes below 48 px.

## Web integration

Copy the `svg`, `app-icons` and `web` folders together to retain the supplied relative paths. If your build flattens the assets, adjust the paths in the CSS, manifest and HTML snippet. The manifest defaults to an application hosted at `/`; set its `id`, `start_url` and `scope` to your actual app route when integrating it.

For a site that follows system appearance:

```html
<picture>
  <source srcset="/brand/svg/on-dark/porthole-horizontal.svg"
          media="(prefers-color-scheme: dark)">
  <img src="/brand/svg/on-light/porthole-horizontal.svg"
       width="240" height="66" alt="Porthole">
</picture>
```

For manual application themes, select the asset using the actual surface theme. `auto` SVGs and the `<picture>` example follow system appearance; they cannot detect the background or your application's theme switch.

The SVGs in `current-color` inherit CSS `color` only when inlined. An SVG loaded through `<img>` does **not** inherit the parent page's color. For a monochrome icon, use the included CSS mask:

```html
<link rel="stylesheet" href="/brand/web/porthole.css">
<span class="porthole-mark" role="img" aria-label="Porthole"></span>
```

The helper follows `prefers-color-scheme`, supports `data-theme="light"` / `data-theme="dark"`, and uses the system text color under forced colors. A component's `color` can override the mark locally. Use `aria-hidden="true"` when a neighboring label already says “Porthole”.

For a manually themed wordmark or lockup, the CSS also includes `.porthole-logo-light` / `.porthole-logo-dark` classes. Put `data-theme` on the document root or a containing surface. [web/integration.html](web/integration.html) contains the HTML snippets.

## Size and spacing

- Full icon: at least 48 px; use the small mark below that size.
- Wordmark: at least 140 px wide.
- Horizontal lockup: at least 240 px wide.
- Stacked lockup: at least 180 px wide.
- Leave at least one main stroke width clear around the visible artwork. The supplied SVG canvases include padding.
- Scale uniformly. Do not stretch, rotate or move the cursor or reflection stripes.
- Keep the approved lettering. The system sans-serif used in this guide and preview is supporting text, not a replacement logo font.

The neutral palette is in [brand.json](brand.json). High-contrast variants keep the entire logo in a single color. The reflections are decorative glass highlights, not text or a status indicator.

## Source and rebuild

`source/approved-reference.png` records the approved design. `source/transparent-master.png` was prepared with built-in image generation; the exact prompt is in `source/imagegen-prompt.txt`. The vector master is `source/paths.json`, traced into cubic Bézier paths with a subpixel fitting tolerance. All themes and output layouts reuse this one vector master. The vector outlines are a production conversion of the raster artwork, not the original editable font.

Rebuild with the system Python, Pillow and Pycairo available:

```sh
python3 branding/porthole/tools/build_kit.py
```

To deliberately regenerate the vector master from the transparent raster:

```sh
python3 branding/porthole/tools/build_kit.py --trace
```

The script renders the vector paths with Cairo, encodes platform icon containers, checks the SVGs, alpha channels, icon frames and stripe positions, then packages `output/porthole-brand-kit.zip`. Keep Pillow and Pycairo available if rebuilding on another machine. The supplied exports work without Python.

PDFs retain vector paths in RGB; arrange any printer-specific CMYK conversion with the printer. ICNS files are supplied as icon containers; signing and application-bundle integration are separate packaging work.

See [validation.json](validation.json) for the export checks and [checksums.sha256](checksums.sha256) for the file inventory. This kit is provided separately from the application; it does not change runtime application code.
