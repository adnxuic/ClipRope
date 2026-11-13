# Testing and acceptance

The [rope label task](../../Tasks/CLOUD-rope-labels.md) covers centered names and total counts, circular overview paging, atomic collector changes, plaque gestures and restart. Use the named checks there together with the existing keyboard, rope-race and schema suites. Visual checks must include Chinese and ellipsized names and transparent pixels outside the plaque.

The [card deletion fixture](../../Tasks/CARD-delete.md) checks both settings from their desktop controls, their commit boundaries, permanent deletion inside ReBin, and clipboard following after removing the current head. Run it with `pwsh -File tools/build.ps1 -Action Test -TestRegex '^CARD-DELETE[.]'`.

The [ReBin task](../../Tasks/REBIN.md) adds the tenth rope-menu action and checks recycle, switching and restart. The [rope menu task](../../Tasks/ROPE-MENU.md) covers the other menu actions, post-creation switch confirmation and failures, per-rope density and
fractional anchors, compact image/drag geometry, and history recovery.
Use temporary stores and the named desktop cases for both menu hit regions
and view modes; storage-only checks cannot qualify the visible menu.

The Windows probe suite has CTest registrations and a [measured Excel range](../compatibility-matrix/p0-excel-range.md). The [phase ledger](../../Tasks/README.md) identifies the stage reached by each suite. Use synthetic clipboard payloads and isolated data directories. Describe expected results in English. For each run, record the revision, environment, input, action, observed result and pass/fail status. Mark an unavailable application or environment as **untested**.

The [card menu qualification](../../Tasks/MENU-card-menus.md) covers all six menus, source-file selection, screenshot provenance and managed image lifetime.

## Suite ownership

| Directory | First evidence to collect |
| --- | --- |
| `tests/unit/` | Exercise the domain commands against small, deterministic histories. Check final order, identity, capacity and side effects. |
| `tests/integration/` | Use a temporary real SQLite database and Blob directory. Exercise commands through the application service, close all connections, reopen and compare durable state. Include clipboard adapter and window integration on Windows. |
| `tests/clipboard_sources/` | Provide a controllable Windows `IDataObject` producer with deterministic bytes and configurable delays, blocked reads, empty formats, failures, `IStream` and `IStorage`. |
| `tests/compatibility/` | Keep reproducible clipboard, Office and desktop scenarios. Store versioned Office observations in `docs/compatibility-matrix/`. |
| `tests/fault_injection/` | Interrupt specific storage boundaries, inject I/O failures and re-open isolated histories. Exercise the external capture timeout/isolation path when available. |
| `tests/performance/` | Measure latency, resource peaks and long-run behavior from repeatable workloads at recorded cardinalities. |

Run the domain and storage suites on each commit once the targets exist. Select the remaining suites by the boundary touched. A successful configure or a test binary with zero discovered cases does not establish a passing suite. See [build instructions](build.md).

## Core behavioral oracles

Lists below run from head to tail. Start each row from the stated setup and verify the result through the read API. Add persistence-boundary assertions where a row mentions commit, failure or restart. Product behavior follows the [algorithm contracts](algorithms.md) and [data model](data-model.md).

| Case | Action | Expected evidence |
| --- | --- | --- |
| ORD-1 | With capacity 3 and `A,B,C`, activate C, then capture D. Separately reset and drag A to the tail before capturing D. | Activation yields `C,A,B`, then `D,C,A` with B evicted. Drag yields `B,C,A`, then `D,B,C` with A evicted. Verify contiguous display numbers and that the expected pre-trim tail is the only victim in each run. |
| ORD-2 | Fill three ropes to 100 and add to the middle rope. Move an entry from another rope to its head. Inject a write failure on a separate move. | Each successful arrival trims only the target's current Hanging tail. The move keeps the ID and updates both ropes together. A failed move preserves the original membership and order. |
| FLT-1 | Detach X from a full rope A, capture a replacement, then shrink, clear or delete A in separate runs. Hide the workspace, exit and reopen. | A counts 99 immediately after detaching from 100. X persists with null rope and order, keeps its content and layout, and survives each rope operation. Explicit **Close and delete** removes X. |
| FLT-2 | With B active and full, click X's return button. Repeat by changing active rope after click, by dropping between two entries on C, and by deleting the frozen target before commit. | The button returns X to B's head. An explicit drop places X at the previewed index on C. Both keep X's ID and trim only the target. Lost target or failed commit leaves X accessible as Floating. |
| ID-1 | Copy X across ropes, detach the copy, delete X and run GC. In a fresh run, edit the detached copy while X still exists. Attempt to delete the main rope. | The Hanging and Floating entries have distinct IDs and initially share one snapshot. The surviving Floating entry can still restore the shared bytes after GC. Editing replaces only the edited entry's snapshot. The main rope survives. |
| CLP-1 | With a text box selected, invoke the rope-head command or select a tray preview. Separately select a card, scroll, switch ropes, reorder or click the Floating return button. | Rope-head and tray commands publish the chosen card's saved formats for manual paste. The field stays unchanged. A committed change to the active rope or its head copies the new head once under the matching setting. Check all four switch combinations, saved head edits, no-op and canceled gestures, inactive ropes, empty heads and publication failure. Paste manually into the field and verify no duplicate history. |
| CLP-2 | Double-click a Hanging card and observe listener events. After publication, copy identical bytes again from an external source. | The self-write creates no capture, second promotion or extra trim. The new external copy creates its own entry. Check delivery, order and failure outcomes using the double-click cases below. |
| CAP-1 | Use the controlled source to change clipboard owner mid-read, fail one or all formats, block a request, and switch or delete the capture target during delay. | Each accepted event commits at most once to its bound rope. A partially saved card advertises the usable formats. Total failure produces no successful card. Mixed-owner formats and skipped late results are discarded. Deleted targets receive no fallback. |
| LIFE-1 | Publish, start an export or begin an external drag where supported. Delete/evict the last owning entry while the operation still uses its Blob. Release the lease and run GC. | Readers finish using leased bytes. GC can collect the bytes only after the last durable reference and runtime lease are gone. |
| VIEW-1 | Change card color or font size, then edit rich text as plain text. Race an edit or thumbnail result against deletion or content replacement. | Appearance changes leave the saved formats intact. Saving edited text replaces the edited entry's snapshot and drops stale representations. Stale results cannot recreate entries or paint the wrong slot. |

## Card double-click on Windows

Use a controlled destination process with two editable text boxes and a read-only control. At the start of each run, record the selected control, caret or selection range, and initial field values. Use a card with a unique synthetic text marker and at least one other saved format. Observe clipboard formats, destination edits and committed rope order independently. Use Hanging cards for native paste. Exercise Floating content actions separately below.

| Case | Action | Expected evidence |
| --- | --- | --- |
| FLOAT-1 · Content actions | Double-click each saved-content type beside a full rope. Exercise focus loss, save/cancel, a failed save, missing files and the cross-directory picker. | Verify the actual editor contents, opened file or Explorer selection. Compare clipboard sequence, external field selection, full rope order, card geometry and workspace visibility. [Floating contract](../decisions/0008-floating-double-click.md) |
| DBL-1 · Selected text box | Place the caret in box A, open the workspace with the hotkey and double-click a Hanging card. Repeat with selected text. Exercise focus changes caused by opening the workspace and by the first click. | The gesture retains A as the intended target. It publishes the saved formats first, then inserts the content once at A's caret or replaces its selection once without user Ctrl+V. Box B and the read-only control keep their original text. |
| DBL-COPY · Copy only | Select an external field and double-click a non-head Hanging card with the action set to Copy only. Then manually paste. | The card moves to its own head and the workspace collapses. The field records zero automatic edits and one edit after manual paste. |
| DBL-2 · No editable selection | Begin with no editable target selected. Double-click, select a destination and manually paste. In a separate run, copy Y between the double-click and Ctrl+V. | No field changes automatically. Manual paste receives the published content in the first run and Y in the second. Clipboard publication needs no selected field. |
| DBL-3 · Target validation and format choice | Switch from A to B before dispatch, destroy A, mark it read-only, or use a password field. Separately offer image, file-group and HTML-only cards to a verified writable editor. | Invalid targets receive no shortcut. A valid editor receives exactly one. Compare the actual attachment data or lack of edits to ordinary native paste. |
| DBL-4 · Dispatch and consumption | Have the target accept Ctrl+V in one run and discard it in another. Exercise partial and zero keyboard dispatch. | A complete batch reports shortcut_sent for either consumer result. Count actual edits independently. A partial batch reports unconfirmed and never resends paste. |
| DBL-8 · Custom editing surfaces | Double-click a Hanging card with focus on a value-only Group or an opaque custom surface. Exercise Unicode, HTML, RTF, text mixed with pixels and a switch between two Groups sharing an HWND. | Count the actual shortcut and edit independently. Pure text formats paste once. The mixed offer and the changed field receive zero shortcuts. Compare saved Markdown against manual paste in rendered and source views, at a caret and over a selection. |
| DBL-7 · Native application matrix | Perform physical Hanging card double-clicks into Notepad, Edge, Word and Excel. | Inspect final text and selection, rich formatting, or actual cell values in the target application. [Fixture and results](../../Tasks/PASTE-native-paste.md) |
| DBL-5 · History and visibility | Start with the workspace visible. Double-click a non-head Hanging card. Exercise successful, skipped and unconfirmed deliveries. | A committed promotion moves the Hanging card to its rope head. The visible workspace collapses after commit. Starting from the tray keeps the workspace hidden. |
| DBL-6 · Publish and commit faults | First fail clipboard publication. Then publish successfully and fail the history write. Repeat the second run with successful, skipped and unconfirmed delivery. | Publication failure leaves the field, membership and order intact with zero delivery attempts. After publication, report the actual delivery and commit results separately. Failed promotion preserves the previous card state and window. A successful insertion stays in the field once. Recovery leaves any later clipboard owner alone. |

## Storage and recovery

Inject interruptions during staging, after Blob installation, inside the SQLite transaction, immediately after commit and during GC. Fully restart before inspecting the database and Blob tree. Require either the old entries and old tail or the complete new state including trimming. A pre-commit file can be an orphan. A record reported as healthy must resolve all its advertised bytes. Verify detection and visible reporting for a deliberately missing or damaged Blob.

Inject disk full, permission denial, database corruption and unsupported format versions. A failed new capture must preserve the history it would have trimmed. Exercise a backup with shared and Floating-only Blobs, an export racing GC, and restore into a fresh directory. Verify that an unsupported version leaves the source and restore target intact. A terminated process is a crash test. Record any real power-loss test separately, including storage hardware and flush settings.

Folder paste acceptance checks the original file, shortcut, selection and directory tree in the destination. Decode screenshot output to verify native PNG bytes or full-size DIB pixels and alpha. Navigate between presses and inspect both directories for unintended writes. [Folder paste decision](../decisions/0005-explorer-folder-paste.md)

Image file acceptance checks both the actual copied file and its independent pixel publication. Exercise Hanging paste against an Explorer folder, a browser composer, a desktop attachment editor and a plain editor with a selection. Compare manual and card paste using original PNG filenames and bytes, and check address-bar exclusion and changes of editor between presses. [Image file qualification](../../Tasks/IMG-image-files.md)

## Clipboard, privacy and Windows desktop

- Use the controlled source for format selection, size limits, malformed DIB dimensions, HTML encoding/length, file-list bounds, invalid streams and backlog handling. Verify that previews cannot run scripts, fetch remote resources or activate OLE objects. Start with an existing card. With capture paused, an excluded app or an explicit history-exclusion marker, verify that the event adds zero entries and preserves the existing card. A cloud-only exclusion on an otherwise eligible event still permits local capture.
- Exercise Unicode, HTML, RTF, image pixels, file references and ordered multi-file groups. Check native cut/paste while ClipRope observes, copy semantics on history restore, and missing source files. On exit, test a current self-owned clipboard versus a clipboard already replaced by another owner.
- On a real Windows desktop, place a separate process under a transparent gap and record which window receives the click. Repeat after the card moves. Check card controls versus double-clicks, long press/drag cancellation, local Tab editing, Ctrl+wheel versus wheel routing, hotkey conflicts, tray collection and Floating interaction while the workspace is hidden.
- Exercise mixed DPI, negative display coordinates, taskbar moves, monitor removal, `WM_DPICHANGED`, RDP reconnect, graphics-device loss and sleep/resume. Check rescued Floating windows are visible, viewport anchors survive head inserts and reused draw slots show only results matching entry ID and content version.

## Card rendering

`VIS-01.hanging_layout` checks the rope curve, the rotated card frame used for hit testing and the swing spring. `VIS-02.card_gallery` paints one synthetic card per template on a private desktop. Each hanger must carry its number and type icon. Each paper must show plain paper above its content. Hover must change only the hovered card. `cliprope_gallery_test --frame <rope.bmp> <floating.bmp> <hover.bmp>` writes review frames for comparison with the [hanging card decision](../decisions/0003-hanging-card-surface.md).

The gallery also samples the opening swing and compares neighboring hangers pixel-for-pixel with the committed order during insertion. `P3-01.floating_surface` checks immediate movement, Floating template changes at the rope boundary, Escape, capture loss and intermediate landing frames. `P3-03.floating_desktop` checks a middle slot before button-up and after commit with another process in the foreground. The geometry test measures the grip error over negative screen coordinates and several DPI scales. [Motion acceptance](../../tests/compatibility/hanging-visuals.md#card-motion-and-dragging)

`HOV-01.desktop_hover` moves the physical pointer across a transparent gap and checks the on-screen close marks after it stops. Run it with `CLIPROPE_TEST_DESKTOP_INPUT=ON`. [Rapid hover acceptance](../../tests/compatibility/workspace_surface.md#rapid-hover-across-transparent-gaps)

## External drag copy

For Ctrl-drag, run `DRAG-03.external_drag_copy` and the interactive application driver. The receiver checks actual data across processes. The application matrix checks the resulting document or filesystem. [External drag qualification](../../Tasks/DRAG-external-copy.md)

## Office matrix

Test Excel values, formulas, formatted/merged cells and charts. Test PowerPoint text boxes, grouped shapes, charts and whole slides. For **each object**, run direct native copy/paste with ClipRope observing, history restore after overwriting the clipboard, and history restore after source-app exit and ClipRope restart. Record source and destination application/build, Windows build, object kind, path, offered/saved formats, observed content, style and editability, and failures. Use a synthetic workbook or deck. Mark native-object restoration as verified only for the tested adapter and scenario. See [format conventions](formats.md).

## Performance targets awaiting measurement

Use Release x64 without a debugger. Record Windows, CPU, RAM, disk, GPU/driver, DPI, sample count and workload. Sum Private Bytes across **all** ClipRope processes, including a capture worker when present. Report GPU/DWM overhead independently.

| Workload or metric | Proposed reference target |
| --- | --- |
| Settled tray, zero Floating | Total Private Bytes ≤ 50 MiB |
| One rope, around 12 visible mixed text/image cards | Total Private Bytes ≤ 100 MiB |
| CPU thumbnail cache / GPU content cache | Initial byte caps of 16 MiB / 32 MiB with separate accounting for render buffers |
| Idle on reference machine | 60-second mean CPU < 0.1% and no continuous redraw |
| Cold start to usable tray and listener | p95 ≤ 1 s |
| Resident workspace expansion / controlled text copy to durable commit | p95 ≤ 100 ms each |
| Scrolling at 60 Hz | 16.7 ms frame budget, measured at p95 and p99 with a dropped-frame count |

With fixed rope capacities and a fixed Floating population, repeatedly capture/trim, switch ropes, scroll and detach/return. Sample Private Bytes, Working Set, GPU use, handles, threads, disk Blob counts and outstanding leases before, during and after settling. Separately vary Floating count, image size and Office delays. Report peak decode memory, blocked reads and disk-full failures separately. Track expected persisted data and visible windows when interpreting growth.

The [milestone gates](maintenance.md) identify which evidence is required before advancing.

For rope gestures run `P4-04.virtualized_overview`, `ROPE-01.hover_wheel`, `ROPE-02.immediate_drag`, `P4-03.management_transactions` and `P4-05.live_rope_races`. The physical wheel and drag cases need an interactive Windows desktop. [Gesture acceptance and reproduction](../../tests/compatibility/overview_surface.md#rope-gesture-qualification)

For keyboard scopes and editable settings run `KEY-01.tab_routing`, `KEY-02.hover_shortcuts` and `KEY-04.configurable`. Use `fluent_visuals.py --shortcuts-only --matrix` for the four-column layout and accessibility checks. The desktop case sends keyboard input with a separate process holding the foreground editor. It checks line/card/gap navigation, complete pass-through outside the scope, repeat suppression, release pairing across hide/show, dialog and drag exclusions, failed writes and restart. The fixture also verifies keyboard-hook installation failure and retry. [Keyboard acceptance and reproduction](../../Tasks/KEY-shortcuts.md)


## Bilingual desktop

Run `ctest --preset windows-qualification -R '^I18N-' --output-on-failure` for
resource validation, ordered locale/script matching, immutable view lifetime, formatting
and fallback, old-profile preferences, transaction rollback, restart/reset and live
state preservation. The real desktop case addresses stable HWND classes/control IDs,
pauses capture in its disposable profile and never publishes clipboard content. It
compares clipboard sequence, listener identity, card revisions/order, collector, capture
policy, workspace stacking and settings before/after switching. The second desktop
case forces absent preferred fonts. Existing English-caption fixtures set an explicit
English Auto-detection probe; production ignores that probe.

Use [the localization visual fixture](../../tests/compatibility/localization_visuals.py)
for both-language theme/DPI and narrow-window captures. Inspect long confirmations,
Unicode mixtures, glyph fallback, accessible labels and menu/button widths. Message-level
composition tests do not establish physical Chinese IME candidate-window behavior;
check an active composition, caret/selection, focus and drafts on an interactive desktop.
Run both language contexts through the menu latency gate below. Actual results and
remaining environment gates belong in [the stage record](../../Tasks/I18N-bilingual.md).

## Interactive UI performance

For screenshot hover, run `ctest --preset windows-qualification -R '^PREVIEW-'`.
`PREVIEW-01.sandbox` covers source identity/leases, cancellation, bounded decoding,
cache/queue races, preferences and placement. `PREVIEW-03.faults` deliberately runs a
malicious test worker through the production launcher to exercise timeouts, crashes,
invalid IPC and file/network/child-process denial. The fault worker is test-only.
Its controlled network endpoint requires a normal parent token; an already-contained
runner reports CTest skip code 77. Run it outside that runner for qualification.
`PREVIEW-02.desktop` needs the active unlocked input desktop and uses a synthetic
external editor and disposable profile. Do not run other physical-input fixtures at
the same time. [Evidence and remaining gates](../../Tasks/PREVIEW-screenshot-hover.md)

The [preview soak](../../tests/performance/README.md#screenshot-preview-service-soak)
measures the service separately. It does not establish desktop display p95 or aggregate
worker/GPU resource qualification. Physical mixed-DPI monitors, actual lock/unlock and
installed lifecycle checks remain separate acceptance scenarios.

A change to menu appearance, inputs or layout must qualify time to the first usable frame. Attach the Release before/after timings, the first-open and repeat-open distributions, resource creation counts and the matching visual/automation results. Keep database reads, file classification and foreign accessibility calls out of the opening gesture. Measure on the same reference desktop and include the slowest trial with its stage breakdown. [Menu workload and budgets](../../tests/performance/README.md#context-menu-latency)

Windows CI runs `UI-06.menu_resources` on a private desktop. It exercises repeated popup lifetimes, cache invalidation and limits, device target replacement and a pending file menu. The interactive `UI-06.menu_focus` case checks the original and changed fields after menu dismissal. The release benchmark measures physical input and DWM. [Fluent management qualification](../../Tasks/FLUENT-management.md)

## Find in Cards

Run CTest's `FIND-` cases and `VIS-02.card_gallery` in the Windows qualification preset. The storage fixture exercises deep full-text matches, field extraction and position paging. The desktop fixture uses a disposable profile, a native receiver and real input to check key ownership, cross-rope location, edits, deletion and capture. The appearance fixture checks light, dark and contrast at four design DPIs and saves the frames in the qualification build's `find-visuals` directory. [Phase evidence](../../Tasks/FIND.md)

## UI design acceptance

For direct-build cleanup, build `cliprope_build_uninstall_test` and
`cliprope_uninstall_dialog_test` in `windows-qualification`, then run
`ctest --preset windows-qualification -R '^UNINSTALL-' --output-on-failure`.
The core/real-bootstrap cases use disposable Unicode output folders, synthetic
Hanging/Floating history and a unique nested HKCU namespace. Only the opted-in
qualification uninstaller accepts that namespace after validating its fixture marker.
The retail executable rejects fixture overrides. No case uninstalls the user's copy.
The dialog case uses fake cleanup callbacks, native UIA and a private desktop.
Run `python tests/compatibility/uninstall_visuals.py` for both languages at four
design DPIs in Light/Dark/contrast, including keep/delete, pending, retry, disabled
shared history, invalid layouts and long paths. Inspect the images separately from
the fixture result. [Acceptance evidence](../../tests/compatibility/fluent/build-uninstall/README.md)

`tools/build.ps1 -Action Test` also runs `BUILD-04.package_registration`. It compiles
the production NSIS uninstall section into a disposable Temp fixture and uses a
unique HKCU namespace. Synthetic payloads cover disabled/enabled startup, missing
and foreign owners, ownership changes, keep/purge, and a foreign installed-app
location. Native history and AppContainer calls are represented by local sentinels;
the fixture does not access real history, uninstall the daily copy or use the
clipboard. The pinned NSIS compiler is shared with packaging. The complete native
package lifecycle remains covered by the separate empty-account installation rehearsal.

Every UI change follows the [design contract](ui-design.md) and
[mandatory checklist](../../.agent/ui-checklist.md). Run `UI-18.source_contracts`,
`UI-18.scanner_tests`, `UI-17.components` and affected native integration tests.
`I18N-04.desktop` covers real arrow/list messages, the zero-height dropdown regression,
native Selection/ExpandCollapse/RangeValue, exclusions limits and failed-save drafts.
`COLOR-03.editor_preview` checks paper preview text, selection and undo. Shared button
tests check independent state differences and stopped animation timers.

Use `python tools/check_ui.py --base <base-commit>` to verify the changed-source evidence
link. [Evidence schema](../../tests/compatibility/fluent/evidence/README.md). Preserve
before/after screenshots, a separate review conclusion, raw resource/timing observations
and explicit open physical checks. The 24 synthetic language/theme/DPI contexts do not
replace actual mixed-monitor, Windows high contrast, keyboard or IME qualification.
