# ADR 0003 · Hang cards from a full-width rope

**Accepted · 2026-11-01.** The workspace draws each Hanging card as a steel S-hook, a light wooden hanger and a sheet of paper on one sagging hemp rope. The hanger carries the card's number and type. The paper carries only the content. [Visual follow-up](../../tests/compatibility/hanging-visuals.md)

## Source

The contributor supplied a reference picture and a staged plan in Chinese on 2026-11-01. It opened with 「理想的GUI是图1，但是目前的实现（图2）很粗糙，根据以下计划（计划只是大致方向，具体需要实时调整）优化：」 and asked for a perceptible, restrained sag: 「做'可感知的下垂'，不要做'夸张的拱形舞台效果'」. The plan ordered the work as rope, hanger labels, a clean paper, per-type paper templates, then hover and swing. It set these targets:

- a braided light-brown rope without stripes, sagging 20–45 DIP across a 1920-pixel display with both ends at the screen edges;
- `#n/capacity` on the left of the hanger and a 12–16 DIP monochrome type icon on the right, in 11–13 DIP semibold `#5E5144` text and `#6C5A4C` icons;
- a warm paper without technical labels, images centered with margins, status as a small mark or on hover, 4–6 lines of text with a fade, and file cards with an icon, name, type and path summary;
- a card width-to-height ratio of 0.75–0.9, split into 12–16% hook, 10–14% hanger and 70–76% paper;
- vertical cards, a temporary swing after scrolling and dragging, and a slight lift on hover.

## Surface

The workspace HWND is always `WS_EX_LAYERED`. Each frame is drawn into a premultiplied 32-bit DIB and presented with `UpdateLayeredWindow`. Windows passes input through a layered window wherever a pixel's alpha is zero, so the gaps between papers, the space under the rope and the unused height reach the window below without a per-card HWND. The window region remains a coarse superset made of a polygon around the rope and one padded rectangle per realized card. It limits the area that Windows tests per pixel.

A hidden layered window reappears with its last frame. The workspace presents the new layout before showing the window and before reporting updated card positions after a scroll, resize or mode switch. A lost render target leaves the layout pending until a successful paint installs its alpha map. `WM_PRINTCLIENT` composites the still scene, with no hover or swing, over the caller's DC. Floating windows keep an opaque copy until their opacity drops below one.

The image-body, spacing and row-height rules below are superseded for adaptive cards by [ADR 0009](0009-adaptive-image-cards.md).

## Geometry

The original 24-DIP placement below is now the default for a new rope. Each rope remembers its single-view height. Transparent gaps within a row accept the wheel and pass clicks through. [Rope gestures](0010-rope-gestures.md)

The window spans the full work-area width, 24 DIP below its top. `RopeCurve` is the catenary \(y = e + a(\cosh\frac{w}{2a} - \cosh\frac{x - w/2}{a})\) through both window edges 34 DIP below the window top. Its sag is \(\min(24, w/64)\) DIP, 24 DIP for the 1,536-DIP width of a 1920-pixel display at 125%. Each hook rests on the curve at a whole DIP, so card geometry scales exactly with DPI.

`presentation::Hanging` defines every vertical measurement relative to the rope centerline above the hook. The renderer, hit testing and the window region all read these values.

| Part | Extent in DIPs | Share of the 197-DIP card |
| --- | --- | --- |
| S-hook over the 9-DIP rope | From −7.3 to the hanger at 18 | 12.8% |
| Wooden hanger, 180 wide | 18 to 42, with 25-DIP ends | 12.2% |
| Visible paper, 172 wide | 42 to 190. The sheet is tucked 4 DIP under the hanger. | 75.0% |

The paper's width-to-height ratio is 0.87. A row is 264 DIP. Overview rows use the same pitch.

## Labels and paper

The hanger's left end shows `#position/capacity` in 12 DIP semibold `#5E5144` above a light engraving. The right end shows a 13-DIP line icon in `#6C5A4C` for text, link, image, file or folder, or a warning triangle for unavailable content. Formatted content uses the text icon. An image-only card uses the image icon whatever its summary text.

White paper is drawn as warm `#FBF7EF` with a faint fiber texture. Other background colors are drawn as chosen. Rope color zero, the schema default, selects natural hemp. Any other color tints the same twisted rope. The paper has no header:

| Template | Paper content |
| --- | --- |
| Text | Saved style and font. A fade marks text beyond the last visible line. |
| Link | The host in bold and the wrapped address |
| Image | The preview fitted and centered within the margins. Upscaling of two times or more uses nearest-neighbor sampling. |
| Image file or all-image file group | A fitted thumbnail and filename, or an ordered four-image collage and count. The hanger keeps its file icon. [Image file decision](0004-image-file-previews.md) |
| Other files and folders | Document or folder art with an extension badge, the name, its type or item count and an elided folder path |
| Formatted, unavailable | A centered notice |

A partial or damaged card shows an amber or red dot in the paper's lower-right corner. Hovering a card shows its capability summary, a close control and a deeper shadow. The shadow spreads less than half the gap between papers, so that gap still passes input through.

## Motion

Cards hang vertically at rest. Opening the workspace gives neighboring cards alternating impulses. A visible new head, a wheel scroll and a drop also excite the underdamped swing about the hook. Each card carries its own angle and angular velocity, with a ±4.5° limit. A nominal 16-ms timer presents the motion and stops when it settles. Hit testing uses the same pivot and rotation as painting.

A press fixes the local grip and captures the pointer on a nonactivating input window covering the virtual desktop. Motion past the system drag distance lifts the card immediately. A stationary press and release stays eligible for the system double-click gesture. A separate click-through window paints the carried card with its rotated grip at the pointer using the destination monitor's DPI. Horizontal motion also imparts angular velocity.

During a move the source sheet leaves the rope and its neighbors close up. Crossing a neighbor's center opens a slot at the pointer. The dragged hanger and its neighbors display their projected ranks. The same projection determines the card positions, labels and visible region. On a full target the last available slot accepts the incoming card and the preview omits the displaced tail. Ctrl+copy across ropes preserves the source order.

A Floating sheet entering a visible rope lane changes to the hook, hanger and numbered paper in its existing window. Moving out restores its Floating appearance. Releasing in a lane commits the previewed position, including for cross-rope moves and copies. A Hanging card released outside the lanes becomes Floating. Cancellation or capture loss restores the original layout and order. The return button and double-click retain their head-insertion rules.

## Consequences

- At regular density at most \(\lceil (w - 28) / 200 \rceil + 3\) cards are realized per row for a window \(w\) DIP wide. The [full-width qualification](../../tests/compatibility/hanging-visuals.md#performance) measures compact density separately.
- Image previews decode to at most 320 × 256 pixels, a card's picture area at 200%. Cached previews use the `THM2` header. Older files are ignored and decoded again on demand. The renderer still keeps at most 64 preview bitmaps within 32 MiB.
- The renderer caches the rope at its current width, color and DPI. While cards swing, it reuses up to 8 MiB of rasterized card fronts and draws the rear of each hook behind the rope. Changes to a card's displayed content, preview, number, capacity, style, width or DPI replace its cached front. Translation alone moves the carried window using its existing pixels. Hiding the workspace releases the rope and card-front bitmaps even when a Floating window keeps the renderer alive.

`VIS-01.hanging_layout` checks the curve, card frame and spring. `VIS-02.card_gallery` checks the labels, the plain paper and hover. `P2-02.workspace_surface` checks cross-process click-through and the realized-card bound.
