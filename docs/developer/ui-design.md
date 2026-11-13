# ClipRope UI design contract

This is the normative design contract for new and changed Windows UI. It applies to
management windows, the workspace, paper controls, transient popups and error paths.
Use the [execution checklist](../../.agent/ui-checklist.md) for every UI change and
record acceptance in the [Fluent ledger](../../Tasks/FLUENT-management.md).
Rendering belongs to `platform_win`; domain commands and persistence stay independent.

## Choose a shared component before creating an HWND

| Purpose | Required implementation |
| --- | --- |
| Window, prompt, field, action | `fluent::Surface`, `dialog_chrome`, `CardPart`, native child controls |
| Command popup and composited frame | Existing popup hosts and `CardMenuFrame` |
| List or choice, including the expanded popup | `fluent::list_box` / `choice_box` in `fluent_list.hpp` |
| Overflow | `fluent::ScrollBar`; use an independent bar provider for native lists |
| Color selection | ADR 0023 picker; launcher retains preview and HEX value |
| Inline paper action | `Surface::paper_style`, semantic icon, current `PaperInk` |
| Short help or error feedback | `fluent::Tooltip::bind` / `track`; never Common Controls tooltips |
| Screenshot enlargement | `ScreenshotHover`; suppress the metadata tooltip while offered |

Native Edit/Rich Edit owns typing, selection, undo and IME. Native owner-drawn lists
and choices retain HASSTRINGS, textual items and normal LB/CB notifications.
[Microsoft documents HASSTRINGS accessibility](https://learn.microsoft.com/en-us/windows/win32/winauto/exposing-owner-drawn-combo-box-items).
Do not replace the native Selection or ExpandCollapse provider with a Scroll root.
Query the actual expanded list's Selection and the combo's ExpandCollapse in tests.

## Visual roles and metrics

Use `Palette` roles (`panel`, `subtle`, `line`, `text`, `muted`, `accent`, `on_accent`),
`Font` roles, `px`/`design_dpi`, and the shared Direct2D renderer. Paper/wood material
colors are content, not replacement UI palettes. Preserve the paper texture below
inline actions; high contrast may use system colors for legible control states.

Use `Icon` from `fluent_icons.hpp`, never text arrows, multiplication signs, chevrons
or private-use code points at callers. The registry follows
[Microsoft's Segoe Fluent Icons names](https://learn.microsoft.com/en-us/windows/apps/design/iconography/segoe-fluent-icons-font).
Chevron up/down, close and check have shared geometry fallback when the font is absent.
All icon actions need localized accessible names even when their captions are hidden.

List rows are 32 DIP high with 8 DIP horizontal text inset and 6 DIP corners. Panel
padding is separate from row padding. Expanded choices measure item count against
the available monitor work area; the final item must be reachable. Preserve the
existing compact menu and 20 DIP paper-action dimensions. Do not enlarge a compact
surface by copying Settings row sizes. Shared render helpers own antialiasing,
focus outlines and themed borders; normal application drawing must not add GDI focus
rectangles or independently styled button outlines.

## State and input matrix

| State | Required result |
| --- | --- |
| Normal | Legible role-based text, correct material and border |
| Hover | Subtle rounded highlight; no selection, save or focus side effect |
| Pressed | Distinct feedback; one command on the native activation path |
| Focused | Rounded visible ring for keyboard focus; preserve native focus ownership |
| Selected | Persistent selection separate from hover and focus |
| Disabled | Legible muted state; no hover activation or command |
| Expanded | Styled popup, correct z-order, work-area bounds and keyboard dismissal |
| Empty / overflow | Deliberate empty state; shared scrolling and reachable final item |
| Error / pending | Explain the outcome locally; keep recoverable input and a retry path |

New hover and press transitions last 100 ms and 60 ms. Respect Windows client-area
animation settings and high contrast by switching immediately. Stop timers and
invalidations when settled. Repaint/device recovery must not issue commands.

Exercise mouse click, Tab/Shift+Tab, arrows, Home/End where applicable, Enter, Space,
Escape and wheel. Preserve Ctrl+S, IME composition and undo in paper editors. Opening
Find must retain its IME/navigation/focus restoration contract. Escape cancels a
draft or popup according to its existing command contract, not a generic new save.

Tooltip hover delay is 500 ms; explicit error/focus feedback may appear immediately.
It must not activate, focus, capture or intercept clicks. Hide on departure, wheel,
drag, dismissal, target movement/destruction or owner deactivation. Measure wrapped
localized text inside the work area and expose a Tooltip role/name and open/close
events. Do not show metadata help together with a screenshot enlargement.

## Accessibility, localization and layout

Keep native accessible names, selected/disabled state and Invoke, Selection,
ExpandCollapse, Value/RangeValue or text patterns appropriate to the control.
Provide result notifications where changes are not discoverable through native
events. Disconnect custom providers at destruction, including retained client references.
Keyboard-only and screen-reader checks supplement pixel inspection.

Use catalog keys for app text, including hints, validation and accessible names.
Keep user paths and text as data. Never use a translated caption to identify commands.
Language changes must preserve drafts, selections, scroll positions and undo. Labels
describe the current interaction: use Color for a visual launcher; precise input
format instructions belong inside the picker.

Measure English and Chinese, long names/paths, narrow windows and missing preferred
fonts. Scale all geometry from DIPs exactly once. Recompute fonts, rows, popup bounds
and frame resources after theme/DPI changes. Test Light, Dark and High Contrast at
96/120/144/192 DPI in both languages (24 conditions), including expanded and interactive
states. Synthetic design-DPI captures do not qualify real mixed-monitor transitions,
Windows high contrast, native IME or physical keyboard behavior.

Settings uses a 180 DIP navigation rail, reduced to 64 DIP below a 760 DIP client width. Start the content 28 DIP from the rail and cap it at 660 DIP. Use a single page title, headings for distinct functions and unboxed rows of at least 44 DIP. Show help when scope, consequences or a recoverable problem need explanation. Keep reset in a separate Advanced group. Measure live localized labels and feedback before positioning the controls. The [approved seven-page preview](../../design-preview/global-settings.html) illustrates the spacing and grouping.

Settings choices belong at the trailing end of their row, alongside the label and
description. Prefer concise localized option names and keep the full behavior in
the description/accessibility text. Check the minimum supported width; do not move
a choice below the text solely to accommodate a verbose caption.

Review transient opening frames as well as the settled popup. Native combo expansion
uses an offscreen [WM_PRINT snapshot](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-animatewindow).
Draw the themed nonclient padding into that DC, preserve its client inset and
owner-draw coordinate origin, and defer composited frame fitting until native reveal
finishes. Never overwrite its changing reveal region on every position notification.
The dropdown regression must inspect the native snapshot and frame lifecycle;
a screenshot captured after expansion cannot detect this failure by itself.

## Failure and resource ownership

Record one visible success and one failure before implementation. Failed exclusions
saves keep the draft. Failed immediate Settings writes restore committed values.
Color previews follow [ADR 0023](../decisions/0023-unified-color-picker.md). Cancel,
target invalidation and close must preserve the established transaction boundary.
Painting and resource reconstruction never save, select or repeat a command.

HWND trees own subclasses, timers, tooltip bindings and providers. Drawing resources
remain on their UI thread. Reuse the existing target, formats and bounded caches:
8 MiB shadows, 4 MiB static artwork; transient paper texture storage holds one control
size and contains no editor text. Do not cache user drafts in shared artwork. Release
window resources on close and stop asynchronous callbacks against invalid targets.

## Verification and enforced gates

`python tools/check_ui.py` checks production C++ and RC sources for raw native list/choice
construction, native tooltip construction, GDI focus rectangles, icon characters
and dispersed icon code points. `UI-18.source_contracts` and `UI-18.scanner_tests`
run through CTest; the source gate also runs before the Fluent library builds.
Exceptions in `tools/ui_exceptions.json` identify a rule, exact file and unique
source anchor, plus reason and review condition. Stale, duplicate and broad exemptions fail.

For UI changes, CI runs `python tools/check_ui.py --base <base-commit>` and requires a
changed record in `tests/compatibility/fluent/evidence/`. The record binds source
hashes to tracked screenshots and an English review with explicit conclusions and
open checks. Every checklist category needs an explicit status and evidence/reason;
blank or missing categories fail. Use the schema documented in that directory. A screenshot's existence
proves capture only; it does not prove visual acceptance. Do not mark physical checks
passed from simulations or label unreviewed output accepted.

Record Release first/repeated opening latency, target/resource creation counts and
GDI/USER changes after repeated opens. Menu budgets remain cold p95 <= 100 ms,
warm p95 <= 50 ms and warm p99 <= 100 ms. Preserve raw samples, environment and
interruption results. Pixel comparisons with the same painter are insufficient:
also assert input outcomes, rollback, accessibility and independent state differences.

## Explicit platform exceptions

The system folder picker, IME candidate windows and pre-UI startup failure message
are owned by Windows. Do not skin them. The startup MessageBox call is individually
registered in the scanner. Native input/selection internals and material painting
remain allowed; shared GDI recovery is permitted only when drawing resources fail.
Any additional exception requires a precise call-site entry, documented reason,
review condition and acceptance evidence. There are no directory-wide waivers.
