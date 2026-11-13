# ADR 0009 · Adaptive image cards

**Accepted · 2026-11-01.** A pixel screenshot and a single supported image file share the image body in the workspace, the drag visual and a Floating window. The hanger keeps the image glyph for pixels and the file glyph for a file reference. Image groups keep their ordered collage. Text with a companion image keeps its text template. The publication and ownership contracts are defined in [ADR 0004](0004-image-file-previews.md) and [ADR 0008](0008-floating-double-click.md).

## Measurement

The source size is the oriented pixel size. The display scale in DIPs is the smallest of the horizontal fit, vertical fit and 1.5. Compact fits within 144 × 108 DIP, Standard within 192 × 144 and Large within 256 × 192. Standard is the default. Its height is close to the 148-DIP paper body so images sit at the same reading scale as text. The desktop uses Standard with the transparency checkerboard off, including profiles with legacy overrides. [General settings](0020-general-settings.md) Files, captures and saved representations retain their formats.

A hanging image begins 2 DIP above the bottom of a 30-DIP wooden hanger. Its width is the larger of 160 DIP and the image width plus 12 DIP. The number, glyph and steel hook keep their size. The body, hanger and shadow rotate about the hook contact on the catenary. The Cards page controls a bottom hover caption showing oriented source dimensions in physical pixels on Hanging and Floating images. The tooltip retains file information. Switching off single image file previews selects the ordinary file template and requests availability only. Pixel cards and multi-image collages keep their usual previews. The image extends to its outline, including for a very narrow portrait.

Floating images use a 38-DIP toolbar. Its minimum width is 144 DIP so Return and Close remain accessible beside a tiny image. Restoring a window uses its saved monitor and origin, remeasures the image for the current mode and clamps the resulting window to the work area.

## Rope geometry

Each slot reserves the hanger width plus 2 DIP shadow allowance per side. Adjacent slots have a 24-DIP gap. The viewport reads outward from its anchored entry, fills the visible width and buffers adjacent cards. Wheel detents follow slot boundaries and preserve fractional motion. A preview gap uses the incoming card's measured advance. The preview and committed order share their hook positions and projected numbers.

[Rope density](0013-rope-menu.md) adds a per-rope compact mode: a 12-DIP gap,
half-width image hangers and half-size image bodies, preserving aspect ratio.
The standard image size supplies the regular base geometry. Carried
artwork and target insertion gaps use the same measured density geometry.

Rows that have displayed an image reserve the current mode's maximum image height plus rope, hanger and motion clearance. A paper-only row keeps its 264-DIP pitch. This lets a text-only overview retain its three-row capacity on the reference display. Vertical virtualization uses the actual row heights.

## Preview lifetime

The browser reads the size of a screenshot from a bounded saved header. For a file, the WIC worker reports the oriented size before scaling and reading pixels. Bitmap variants are bounded by monitor pixel size and reuse a versioned derived cache. A missing original can still display a cached variant from an earlier size. Its availability remains separate from the preview. Network and reparse-point reads follow the explicit refresh rule in ADR 0004.

The scrolling view incorporates asynchronous completions in its next wheel or animation frame. Hidden and offscreen cards do not acquire a full-resolution image for layout. The CPU thumbnail cache is bounded at 16 MiB, Direct2D preview bitmaps at 32 MiB and moving card faces at 8 MiB.

[Qualification](../../Tasks/ADAPT-adaptive-images.md)
