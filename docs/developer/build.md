# Building and running ClipRope on Windows

Install the Visual Studio 2022 C++ desktop workload with its x64 compiler, Windows SDK and CMake tools, and PowerShell 7 and Python 3.11 or newer for catalog validation/code generation. Python is not required by the installed desktop. Use PowerShell from the repository root. If CMake is absent from PATH, the following locates the copy bundled with Visual Studio.

```powershell
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmakeBin = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin'
$env:Path = "$cmakeBin;$env:Path"
```

Use the single build workflow below. Extra build trees are rejected. Check the exit code of each operation.

```powershell
pwsh -File tools/build.ps1 -Action Test -Configuration Debug -TestRegex '^P0-01[.]'
```

The P0-01 run on Windows 11 25H2 x64 build 26200.9711 used CMake/CTest 3.28.3-msvc11, the Visual Studio 17 2022 generator, MSBuild 17.10.4, MSVC 19.40.33811.0 and SDK 10.0.22621.0. CTest discovered and passed `P0-01.host_smoke` from a fresh Debug x64 build. The executable starts an OLE STA, registers a message-only window for clipboard notifications, then unregisters and exits. It does not read the clipboard.


## Separate daily and development runtimes

All configurations share `build/cmake` and emit runtime files into `build/app`, with
no configuration suffix. Exactly one `ClipRope.exe` is permitted anywhere in build.
Do not create task-specific trees, renamed desktop baselines or hard links. Every
workflow operation holds a checkout lock; CMake requires the workflow session.

The daily executable is `current/ClipRope.exe` under the current user's registered
install folder. A first installation defaults to `%LOCALAPPDATA%/Programs/ClipRope`.
Use the setup wizard to choose a different empty or new local folder. For silent
setup, put `/D=D:\My Apps\ClipRope` last, with no quotes around the path. An upgrade
or `/ROLLBACK` uses the registered folder even when `/D` names a different one.
Build, Test and CleanRebuild never update it, stop it, modify its real history or
change its sign-in choice. Only the explicit Publish action installs or updates it.
The installer's existing previous-slot rollback remains outside the build tree.

```powershell
pwsh -File tools/build.ps1                           # Production Release
pwsh -File tools/build.ps1 -Action Test               # Mandatory core gate, then Production Release
pwsh -File tools/build.ps1 -Action RunDev             # Independent build/test-data/manual profile
pwsh -File tools/build.ps1 -Action Verify             # Source, cache, payload and uniqueness checks
pwsh -File tools/build.ps1 -Action Package            # Create the tested installer without installing it
pwsh -File tools/build.ps1 -Action Publish            # Explicit verified daily installation/update
pwsh -File tools/build.ps1 -Action CleanRebuild       # Verified archive, exact cleanup, Production Release
```

Package checks the same Production and regression receipts as Publish. To compare
the same verified files, pass `-Action Package -PackageCompressor Zlib` and
`-Action Package -PackageCompressor SolidLzma`. These write to separate folders
under `build/packages`. The default Package output is the canonical solid LZMA
installer. Each installed executable retains its tested Release bytes.

`-Variant Qualification` enables probes; `-Configuration Debug` selects Debug. Both
replace the same development image. Finish with Test, which restores Production
Release even after a test failure and retains the failed test status. Publish
requires a pinned Production Release, matching fingerprints and a successful test
receipt. CI uses `-CI` and cannot produce a publishable receipt.

Test always runs BUILD, core P1, catalog, UI source and uninstall regressions. Use
`-TestRegex` to add cases. Receipts retain the selected scope: the core gate does not
close Office, physical-display, performance or other outstanding release gates.
`-Interactive` explicitly opts into active-desktop, tray and Office configuration;
select the desired cases with TestRegex as well. The daily process must be stopped
for these tests. Prefer private-window-station fixtures; never fall back to the
user's clipboard when the isolated station is unavailable.

An uninstalled desktop without an explicit profile exits with code 2 before opening
history or initializing startup. RunDev supplies the profile and refuses to run
alongside the daily instance. Qualification fixtures can also supply an isolated
local-app-data directory and registry. Multi-copy install/uninstall fixtures live
in disposable Temp roots or an empty test account, never persistent build copies.

CleanRebuild verifies a compressed archive of the complete old build under ignored
`.local-artifacts`, checks resolved paths, reparse points, running images and file
access, then removes only build. Keep the archive until its evidence and manual test
histories are no longer needed. Cleanup is not an uninstaller. A failed compilation
invalidates delivery; an available last-production payload is restored without
certifying it for changed sources. Rebuild and pass Test before publication.

Publish refuses occupied applications instead of killing them, retains the existing
sign-in setting and calls the existing per-user installer. The first migration is
also an explicit Publish. Historical evidence keeps its original archived paths;
these are not current launch instructions.
[Decision](../decisions/0025-single-build-runtime.md) · [Verification](../../Tasks/BUILD-single-runtime.md)

### Uninstall the corresponding runtime

Use Windows Installed apps or `Uninstall.exe` in the daily `current` directory for
the installed package. Its NSIS uninstaller removes the recognized package slots,
Start Menu shortcut and matching installed-app registration. The native
`build/app/Uninstall.exe` removes only its marked development payload; it does not
uninstall the daily application. Their different binaries and appearances are
intentional. Do not copy either uninstaller into the other layout.

Both uninstallers remove only startup values that still name their own executable.
A foreign Run entry or Owner marker does not block removing the selected runtime
while keeping history. It does block shared-history deletion, and the foreign
values and shared preview container are preserved. With the daily uninstall prompt,
choose **Yes** to keep history; **No** requests deletion, which is rejected if another
copy owns startup. An installed-app registration naming a different installation
still blocks the operation before any cleanup. [Regression evidence](../../Tasks/UNINSTALL-daily.md)

## History core

Every normal build validates both [language catalogs](localization.md) and compiles their
UTF-16 tables in the build tree. `CLIPROPE_I18N_STRICT` defaults to ON. Both release
presets enforce it; a pinned release rejects incomplete resources. Run
`python tools/check_i18n.py` to check translations independently of compilation.

The build verifies and compiles the [pinned SQLite source](../../third_party/sqlite/README.md) locally. Use the shared Release x64 tree for the data core. Office tests require the explicit `-Interactive` workflow option. The interactive input test has a separate explicit switch and uses the active desktop clipboard.

```powershell
pwsh -File tools/build.ps1 -Action Test -TestRegex '^P1-'
```

Domain, content, schema, transactional command, version rejection and crash tests create disposable stores. They run on pushes and pull requests through the [Windows CI workflow](../../.github/workflows/history.yml). The [headless executable and OLE round trips](../../tests/integration/headless.md) additionally need a Windows account that can open its noninteractive window station for the fixtures. The [phase ledger](../../Tasks/README.md) records the suites actually run on the reference machine.

## Run the desktop workspace

From the repository root in a Visual Studio developer PowerShell prompt:

```powershell
pwsh -File tools/build.ps1
pwsh -File tools/build.ps1 -Action RunDev
```

Launching the registered daily installation without arguments starts the listener in the notification area. Copy text in another application and open the workspace with the tray icon or Ctrl+Shift+V. A Hanging double-click restores the card and collapses the workspace after successful promotion. Drag to reorder, scroll to older entries and hover a card to show its × for deleting it. The tray menu offers recent entries, Quick note, Global settings and Exit. Ctrl+Shift+Space opens a new Floating text card in edit mode. Settings shows registration conflicts for the two global shortcuts. Both actions are also available from the tray.

The default profile lives in `%LOCALAPPDATA%\ClipRope`. Use `--profile DIRECTORY` for an independent test history. Desktop tests that move the pointer or replace the current clipboard require an opted-in build. Enable `CLIPROPE_DESKTOP_PROBES` for content-free test receipts and `CLIPROPE_TEST_DESKTOP_INPUT` to register active-desktop scenarios. Use a normal build for daily operation.

The same build also writes `Uninstall.exe` and `ClipRope.build.ini` beside
`ClipRope.exe`, including Debug and both Release presets. Exit ClipRope from the tray,
then run that directory's uninstaller to remove the copy and its owned startup values.
History and local backups are kept by default; the window offers explicit deletion
when no other copy owns or uses the shared profile. Libraries, symbols and unknown
files remain. Build the desktop target again to restore the executables and marker.
Keep the marker when moving a complete output. [Removal contract](../decisions/0024-direct-build-uninstall.md)

Drag a card away from the rope to keep it in its own window. The return control puts it at the current rope's head. Dragging it onto the rope returns it there directly. Closing the workspace leaves Floating cards available, and restarting ClipRope restores their saved positions. The × on a Floating card moves the same entry to the head of ReBin.

Double-click a Floating text card to edit it in place. Save or Ctrl+S commits, Esc cancels and Enter inserts a newline. A Floating file or screenshot opens in its default app. A directory or file group opens in Explorer. [Floating actions](../decisions/0008-floating-double-click.md)

Right-click a Hanging text card and choose **Edit text…** to type directly on its sheet, including in the overview. Save or Ctrl+S commits, Esc cancels and Enter adds a line. Scrolling away or hiding the workspace keeps the draft until you return. Saving changed text creates a plain-Unicode snapshot for that card. The card shows a notice before converting rich text. Appearance changes preserve the saved clipboard formats.

Hold Ctrl and drag a card into an editor or folder to copy its contents. This works from the single-rope view and from Floating cards when the overview is closed. Screenshots can be dropped into Explorer as PNG files. Press Esc to cancel.

Use the rope line or its plaque menu to create or manage ropes, choose capacities, or enter overview. Each wooden plaque centers the rope name above `tol:N`, where N is the total rope count. The main name is permanently `main`. The displayed single rope collects new external copies, including while hidden. Overview begins with that collector and follows circular switching order. In overview, drag onto another rope to move a card. Hold Ctrl when starting the drag to copy a Hanging or Floating card to the indicated slot on any rope. A normal wheel scrolls the pointed row, and Ctrl+wheel scrolls the rope list without changing its collector.

To drop a card beyond the visible slots, hold it near the left or right edge of the target row. Keep the pointer there to keep scrolling. Move it inward to stop, then release at the desired slot.

Hover anywhere along a visible rope row, including its plaque, a card or transparent gap, and press **Tab** to select the next collector in circular rope order. In overview, the selected rope rotates to the first row. **Ctrl+Tab** switches between overview and its complete list's first rope in single view, even after scrolling. The external editor keeps its focus. Each press acts once. An explicitly focused workspace also handles these keys outside the hovered row. Settings and inline editors retain their own keyboard navigation. Ctrl+Shift+V is the default global visibility shortcut. [Keyboard scopes](../decisions/0011-keyboard-scopes.md)

Press the left mouse button on the rope line or plaque and drag immediately. In single view the rope follows the pointer vertically. In overview its cards travel with it and neighboring ropes move aside. Release in the desired slot to save the order and select its new first rope as collector atomically, or press Esc to put the rows back. A failed write restores both order and collector. Double-click a rope or plaque in overview to make it the collector and open its single view. In single view Ctrl+wheel cycles through collectors. Both wheel routes work anywhere along the pointed row while the editor retains focus. [Rope plaques](../decisions/0012-rope-clouds.md)

The rope menu contains **Edit style…**, **Back to first card**, **Compact view / Regular view**, **Previous rope**, **Next rope**, **Show ReBin / Hide ReBin**, **Show all ropes / Return to this rope**, **Add new rope…**, **Clear Hanging cards…**, and **Delete this rope…**, in that order. Back to first card only scrolls. Density is saved per rope across mode changes and restart. After creating a rope, choose Yes to switch or No to keep the current collector; No is the default. Shrinking capacity, clearing cards and deleting a rope show affected counts before confirmation. Deletion removes the rope and its Hanging cards, with no transfer selector. Floating cards remain independent. [Menu contract](../decisions/0013-rope-menu.md) · [ReBin navigation and restart](../decisions/0014-rebin.md)

## Check format compatibility

Use the [adapter contract](../schemas/clipboard-adapters.md) and [file-group contract](../schemas/file-groups.md) to interpret the saved manifest. Card information on a card distinguishes saved representations from failed or skipped formats. The original bytes remain in history when a generated image thumbnail is removed.

Use `pwsh -File tools/build.ps1 -Action Test -Interactive -TestRegex '^P5-'` for opted-in format compatibility checks. Stop the daily instance first, or use an independent test session. The Office fixture opens disposable documents on a dedicated test desktop.

The [Office results](../compatibility-matrix/p5-office-2026-11-01.md) separate native paste, history recall after a clipboard overwrite and recall after both applications were restarted. The [file-group test](../../tests/compatibility/file_group.md) exercises the Windows Shell's own Cut and Paste commands.

## Build a release candidate

Use the VS 2022 x64 toolchain above. The release presets select v143 and SDK 10.0.22621.0 and check MSVC 19.40.33811 at configuration. The reference CMake version was 3.28.3-msvc11. All first-party targets use level-four warnings and the static multithreaded C++ runtime. The shipping desktop and command-line recovery tool find ClipRopeWorker.exe beside their own executable. The production preset excludes fault injection.

```powershell
pwsh -File tools/build.ps1 -Action Test
pwsh -File tools/build.ps1 -Action Verify
```

For a release containing popup or shared control changes, also pass the [interactive menu latency gate](../../tests/performance/README.md#context-menu-latency) on the reference desktop before packaging. Archive the CSVs with the visual and automation results.

Every Test invocation requires both `AR3.history_link` and `AR3.history_closure` by name.
The Windows CI selection also exercises `AR3.target_supervision` and `AR3.target_replies`
on its private station. The full historical selection and the physical procedure are
recorded in the [phase handoff](../../Tasks/ARCH-history-and-target.md).

Automatic paste to UI Automation, Office and Explorer targets uses a sibling
`ClipRopeTargetWorker.exe`. Both desktop and headless clients build it. A missing
or incompatible worker leaves the published card available for manual paste. The
release manifest and uninstall inventory include this helper.
[Target worker ownership](../decisions/0027-target-verification.md)

Screenshot hover requires the sibling `ClipRopePreviewWorker.exe`; both release presets
build it through the desktop dependency. An unavailable sandbox or worker disables the
individual preview without an unsafe decoder fallback. Packaging requires the capture, preview and target worker executables. Uninstall removes the preview AppContainer; upgrade/rollback keep its
identity. Validate these changes only with the guarded installer rehearsal in an empty
test account. [Preview implementation and acceptance](../../Tasks/PREVIEW-screenshot-hover.md)

Qualification includes opt-in real applications, input and synthetic clipboard writes. The [packaging script](../../packaging/windows/package.ps1) verifies the portable NSIS 3.13 archive by SHA-256 and writes the per-user installer under `build/packages`. The compiler archive can be pre-staged at `.local-artifacts/nsis-3.13.zip` to build offline. Capture and restore on an installed system use local files and Windows IPC. [NSIS release files](https://sourceforge.net/projects/nsis/files/NSIS%203/3.13/) · [SQLite pin](../../third_party/sqlite/README.md)

```powershell
pwsh -File tools/build.ps1 -Action Publish
```

## Capture privacy

Open **Global settings → Storage & Privacy → Privacy → Edit list…** to manage clipboard capture exclusions. Add executable names such as `devenv.exe` and select **Save** to apply the list. The Shortcuts category supports immediate recording, per-action switches and availability checks. Right-click a rope to edit its name, color or capacity. A shrink, clear or deletion previews the affected Hanging entries before committing. [Fluent management surfaces](../decisions/0016-fluent-management.md)
