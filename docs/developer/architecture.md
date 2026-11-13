# Program architecture

ClipRope targets Windows 11 x64 first, with C++20, Win32, SQLite and a native DirectX rendering stack. The boundaries below guide implementation from P0 onward. The [plan](../../ClipRope_Long_Term_Implementation_Plan.md) treats the host design, worker isolation and performance budgets as engineering proposals. Windows builds, compatibility experiments and resource measurements still need to establish their results.

## Module boundaries

| Module | Owns | May depend on |
| --- | --- | --- |
| `domain` | Rope membership, the two Entry states, content descriptors and rules for ordering and capacity | C++ value types and the standard library. Keep OS handles, database calls and window state outside the model. |
| `application` | Commands, their sequencing, frozen targets, result reporting and ports for history, capture, publication and delivery to a bound text box | `domain` and its own port contracts |
| `localization` | Compiled immutable catalogs, typed keys, locale matching, named formatting and language revision | C++ value types and the standard library; no Windows, database or graphics dependency |
| `persistence` | SQLite writer, indexed history queries, schema validation, immutable blob storage, leases, GC and consistent backup | Application history contracts, domain values, SQLite and filesystem services |
| `platform_win` | Clipboard listener and publisher, editable-target binding and delivery, HWNDs, monitor geometry, tray, hotkeys and native control adapters | Application ports, read-only localization context, Win32/OLE and shell APIs |
| `rendering` | Shared software Direct2D DC target, DirectWrite layouts, layered HWND presentation and WIC previews | Render inputs, read-only localization context, native drawing surfaces and graphics APIs |
| `presentation` | Single-rope and overview view models, Floating controls, gesture arbitration, viewports and visual feedback | Application commands and semantic read models, localization, rendering and window service interfaces |
| `capture_worker` | Reading an external `IDataObject`, selecting and validating supported media, serializing recoverable bytes | Capture contracts and the Windows/OLE APIs for reading media |
| `host` | Executable entry point, process roles, service construction and orderly startup/shutdown | The concrete modules it assembles |

The dependency direction of the business code is `application → domain`. The host supplies the concrete implementations of the application ports. The history store owns authoritative writes. The ordinary thumbnail helper opens the profile database with a restricted SQLite authorizer to check saved representations and serialize derivative installation with collection. Views send commands with stable IDs and consume committed changes. A renderer sees geometry, styles and previews. It cannot edit an Entry or publish a clipboard object. Format adapters can use an internal statically linked registry.

The desktop host owns the language manager and commits preferences before publishing UI-thread
language events. Native controls retain explicit resource bindings; presentation resolves semantic
card labels without changing saved user data. Renderers receive the language context and include its
revision in UI text caches. Domain and command decisions use stable values. See
[Localization](localization.md) for native bindings, fallback and state-preserving refreshes.

## Two paths through the program

**External copy.** The Windows listener reports an update. Application code binds a capture ticket to the clipboard version and active rope at receipt, checks capture policy, and asks the capture STA to read the selected formats. Validated bytes and a manifest go to persistence. One transaction admits the Entry at the head and trims the target rope's tail. A post-commit event such as `CaptureCommitted` carries stable IDs to the view. The tray and workspace can refresh summaries without materializing every representation.

**Hanging double-click.** The Windows adapter binds the original external editor before focus changes. The application publishes the leased snapshot and receives a publication identity. After the independent history attempt, the desktop applies committed window changes and passes the identity to the bound gesture. The adapter rechecks the editor, actual foreground and publication before sending one native Ctrl+V. A failed commit retains the old window and still permits delivery. Report publication, history and shortcut dispatch separately. [Native card paste](../decisions/0003-native-card-paste.md)

**Floating double-click.** Bind the entry on press and revalidate its snapshot, membership and revision on confirmation. Use the menu summary to open the inline text editor, launch or reveal a file target, or show Card information. The desktop-owned inline host binds a transient draft to the selected entry. [Floating content actions](../decisions/0008-floating-double-click.md)

The desktop binds the card × to the current form's deletion preference. The history writer verifies the source and applies erase, Hanging move or Floating rehang in the existing transaction. ReBin entries always use erase. The return button sends `RehangEntryToCurrentRope`. A drop onto a named rope sends `RehangEntryToTargetRope`. The history writer compares collector, entry and snapshot within the manual transaction. The desktop publishes the committed head on its UI STA under the [follow preferences](../decisions/0024-collector-head-clipboard.md). The `ActivateEntryToClipboard` path for `CopyRopeHeadToClipboard` and the tray publishes and promotes a Hanging card without text-box delivery. The head command is unavailable on an empty rope. The tray shows up to five previews and activates the selected entry by ID.

## Execution and lifetime

| Executor | Work and boundary |
| --- | --- |
| Main UI and render thread | Run the window message loop, arbitrate input, update the scene and present. Dispatch slow OLE and storage work to their owners. |
| Desktop command order and storage | The UI STA owns `OrderedCommands` and the primary connection. It binds each deferred command to the gesture and orders it against observed clipboard updates. Capture admissions use a separate writer connection. SQLite transactions enforce membership, rank, capacity and atomic transfers. |
| Capture STA | Run a message pump for calls into external data objects. Pass bounded serialized bytes, checked staging references and metadata across the worker boundary. COM pointers stay in their owning apartment. |
| Clipboard publication STA | Serve the application's own data object and hold its blob leases while Windows or a consumer can request its media. |
| Target verification | The desktop and CLI own native publication and keyboard dispatch. A supervised on-demand helper retains remote target identity across binding and verification. See [target worker ownership](../decisions/0027-target-verification.md). |
| Ordinary previews and outward drag | The desktop schedules bounded file/Blob requests and receives keyed completions. Supervised children inspect authorized file lists, decode thumbnails or encode leased DIB pixels. The STA revalidates a staged drag before invoking OLE. [Worker roles and limits](../decisions/0026-preview-and-drag-workers.md) |

The desktop and headless hosts dispatch foreign reads to a sibling `ClipRopeWorker.exe`. Each worker serves one ticket and runs in a private kill-on-close Windows job. A wedged external `GetData` expires after three seconds. The host can reclaim that ticket and accept a newer version. The P6 comparison records in-process and external-worker recovery times and aggregate Private Bytes. [External capture worker decision](../decisions/0002-external-capture-worker.md)

The desktop additionally owns one screenshot-hover service. Its background thread reads
one leased representation, and its dedicated `ClipRopePreviewWorker.exe` decodes within
an AppContainer and bounded Job. This queue and memory cache are independent of ordinary
thumbnails and capture. Views report candidates and cancellation; they neither open
payloads nor launch decoders. [Screenshot preview decision](../decisions/0022-screenshot-hover-preview.md)

A thumbnail completion identifies a Blob and cache generation. One notification batch carries up to 64 distinct identities. The host checks realized workspace cards and visible Floating windows at their own widths, then refreshes any open file menu or information surface. Explicit refreshes and unknown, mixed-generation or oversized batches trigger a full pass. The screenshot-hover service sleeps until work or its next cache-expiry deadline.

`persistence::Browser` returns bounded facts with its SQL statements finalized. The desktop CardBrowser classifies those values and schedules requested previews after releasing the database mutex. Display has no durable path-kind side effect. An explicit menu observation checks the bound entry, snapshot and revision before writing. Capture prepares searchable RTF/HTML fields through the Windows adapter before it opens the admission transaction. Bytes and fields commit together. The host-owned color session supplies the immutable paint inputs for workspace, carried and Floating cards.

`rendering::CardBrowser` contains a read-only `persistence::Browser`. The desktop supplies rope metadata and anchor reads directly from that reader. Saving an anchor or confirming a menu observation goes through `HistoryStore` with the original command ordering. Scrolling persists only the view anchor. [Projection, retained-menu and scroll verification](../../Tasks/architecture-evidence/OPT-B.md#projection-and-persistence)

Startup opens and checks the local store before presenting saved history. Initialize the unique main rope on first run. Restore the active rope, visible range and independent Floating layouts from committed state. Without an open workspace, `TrayResident` continues capture and keeps Floating windows available. `WorkspaceVisible` also exposes the workspace and its taskbar entry. Explicit exit stops the listener and tears down windows while preserving card records. Check ownership and the result of any clipboard flush during exit. See [Data model](data-model.md) for durable and runtime lifetimes.

## Window, input and rendering contracts

The UI thread shares a premultiplied DIB between surfaces. Independent width and height high-water marks are combined up to 8 MiB. Larger requests use their actual dimensions. Hiding a workspace or Floating surface releases oversized backing after any active presentation finishes. Full-resolution surfaces can exceed the reuse budget while displayed. [4K and mixed-shape measurements](../../Tasks/architecture-evidence/OPT-C.md)

- Draw the workspace in one host HWND. Virtualize vertical rope rows first, then horizontal cards for each visible rope. Read the visible range plus a small margin. A scroll anchor is an Entry ID and local offset. A new head preserves a reader's anchored position, and removal of that anchor selects an adjacent surviving entry.
- Use a tool window for each visible Floating card and share the graphics device across windows. The [Settings Center and management menus](../decisions/0016-fluent-management.md) share a native UI kit. The center receives a settings snapshot plus live commands for card preferences, new-rope capacity, shortcut bindings, appearance and data location. The host resolves the normal profile before constructing its history writer. [Profile relocation](../decisions/0019-appearance-and-profile-location.md) One activatable Rich Edit host overlays the paper inset on either card surface. [Inline editing](../decisions/0015-inline-text-editing.md) Floating geometry is stored in DIPs with monitor information. On DPI changes or monitor removal, recompute visible placement.
- Route a card's gestures before gestures on its rope. Use the system drag distance for cards, grip a rope on press and let Windows recognize double-clicks. Support drag cancellation and commit ownership changes before switching window roles. Scope workspace shortcut commands to a hovered visible row or the explicitly focused workspace body. Dialogs, editors, menus and active gestures keep their input. The row input thread posts navigation commands to the UI thread and pairs each captured shortcut press with its release. Scrolling the overview does not change the capture destination. [Keyboard scopes](../decisions/0011-keyboard-scopes.md)
- Keep the short-lived Windows editor binding outside the domain and persistent history. Exercise changed focus, closed controls and incompatible formats with each adapter. [Native card paste qualification](../../Tasks/PASTE-native-paste.md)
- Transparent pixels and input hit regions require separate handling. P0 must demonstrate clicks reaching an unrelated process through a blank region, including after a card moves. Prove mixed-DPI hit testing on the same host.
- The workspace presents per-pixel alpha through one layered HWND inside a coarse window region. `presentation::Hanging` and `RopeCurve` own the card and rope geometry shared by drawing, hit testing and the region. Hit tests map the pointer into each swinging card's frame. [Hanging card decision](../decisions/0003-hanging-card-surface.md)
- `RopePlaque` supplies the rounded bounds for painting and hit testing. Each frame carries the committed total rope count. Overview maps visible positions to a circular sequence beginning at the collector; a reorder command commits ranks and the new first collector together. [Rope plaques](../decisions/0012-rope-clouds.md)
- During a confirmed hold, `win::CardDrag` owns a temporary input window and a separate card window that can travel outside the workspace. The workspace resolves the release target and issues the existing history command. The renderer consumes card summaries and a screen pose. [Drag and swing behavior](../decisions/0003-hanging-card-surface.md)
- Render on change. Bound CPU and GPU caches by bytes, release workspace-only resources when hidden, and rebuild device-dependent resources after device loss. A color change needs a repaint. Changes to text, width, font size or DPI invalidate text layout. Hangers and skins are decorative resources.

## Shared color editor

The platform-independent model in `domain/color.hpp` owns RGBA/HSV conversion,
parsing, compositing, contrast and semantic selection. The presentation controller
owns Original/Draft/Committed transitions. `platform_win/color_picker.*` supplies
one owned popup that immediately shows the complete color editor and a fixed footer.
The first common swatch is always Default; there is no recent-color view or expansion
mode. HEX and a combined RGB field share the same draft model.
The SV/hue/alpha controls cache Direct2D resources and expose native UI Automation
Value/RangeValue providers. Text fields remain native Win32 edits.

Settings, card menus, rope menus and complete rope properties supply guarded
callbacks. The host applies temporary rendering overrides by stable identity across
single, overview, Floating and open inline editors. Inline color changes suspend
formatting undo and preserve text selection and modification state. Parent menus
and the picker share input ownership. Save errors retain the picker; target
invalidation, Esc, closing and outside clicks cancel it.

See [ADR 0023](../decisions/0023-unified-color-picker.md) and
[qualification evidence](../../Tasks/COLOR-picker.md).

## Trust and persistence boundaries

Clipboard formats, source hints and paths are untrusted input. Limit extraction sizes and queue depth, validate media and render inert previews. Capture may be partial. Publish only the formats that were actually saved. User file references have a separate lifetime from ClipRope's bytes. Diagnostics record outcomes, format names, timings and error codes while excluding clipboard content and full user paths.

Persisted card state has exactly two variants. A Hanging entry belongs to one rope and has a rank in that rope's single order. A Floating entry has neither a rope nor a rank and remains durable across process exit. The storage transaction enforces the rope's limit. The UI reflects its result. Blob reclamation additionally checks both persistent references and live leases. The exact relationships, atomicity boundaries and failure cases are specified in the [data model](data-model.md) and [algorithm contracts](algorithms.md).
