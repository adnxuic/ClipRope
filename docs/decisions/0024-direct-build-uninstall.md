# ADR 0024 · Direct-build uninstall

**Accepted · 2026-11-04.** The contributor requested uninstall support beside the
compiled executable after switching build directories left an older sign-in owner.
This extends the removal workflow in plan sections 15–16 without changing history
ownership or the [first-run sign-in policy](0020-general-settings.md).

## Decision

Every `cliprope_desktop` build depends on a native `cliprope_uninstall` target and an
always-run marker writer. The output directory contains `Uninstall.exe` and
`ClipRope.build.ini` with `Layout=direct-build-v1`. Rebuilding regenerates a removed
tool and marker. NSIS is unnecessary for this path. Direct builds do not register an
installed application; NSIS continues to generate its own installed uninstaller.

The shared Fluent window names the exact directory and defaults to keeping history
and local backups, with Cancel focused. The explicit delete-data switch includes
Floating entries and the current profile's local backups, resolving a relocated
profile through the existing per-user locator. It uses `ProfileLocation::purge`,
including its recognized-profile, active-writer and unexpected-file guards. A foreign
startup/installation owner or another running copy disables data deletion.
Explicit `--profile` directories are independent and are not enumerated or purged.

Cleanup removes only `ClipRope.exe`, `ClipRopeWorker.exe`,
`ClipRopePreviewWorker.exe`, `cliprope_headless.exe`, `cliprope_profile_support.exe`,
`Uninstall.exe` and the direct-build marker from that directory. Libraries, symbols,
other files and the build directory remain. Startup cleanup removes only the Run
value and Owner value that still name that exact executable, including an Owner
marker whose Run value is already absent. Other copies' values and installed Apps
registrations remain. Conflicts in the desktop now identify the other executable
instead of describing every failure as a Windows permission problem.

The launcher verifies and copies itself to one reusable, marked
`%TEMP%\ClipRope.Uninstall\Uninstall.exe`, then exits. The helper waits for that
parent before opening the window, so it can remove the original uninstaller too.
Only this bounded helper and its `helper.id` remain in Temp; no growing per-run
directories or delayed shell deletion scripts are created. A concurrent helper
blocks replacement and yields a recoverable launch message.

## Failure boundaries

A marked installed `current` slot, running target, reparse path, read-only file or
locked runtime file is rejected. Directory handles hold the target and its ancestors
against renaming; verified non-reparse file handles acquire DELETE access before
registry, preview-container or history changes. Matching startup values are opened
for writing before those changes, and ownership is rechecked before a purge. Windows
file disposition operates on the held handles; it never recursively deletes the
build directory. A missing runtime file is harmless, allowing retry after a partial
removal. The marker is removed last.

Preflight failures preserve runtime files, startup values and history. Cleanup across
Windows files, the registry, the AppContainer and the profile is not one atomic
transaction. Once explicit purge or file disposition starts, a later OS failure can
leave partial cleanup; feedback reports failure and permits retry. Cancellation
before confirmation leaves registry, history and the source payload unchanged.
Cancellation/close is disabled while the worker owns an in-progress removal.

## Alternatives and consequences

Requiring NSIS for every local build would leave ordinary developer outputs without
cleanup. A recursive script would put adjacent developer files at risk. The native
allowlist shares registration/profile logic and native UI while preserving those
files. It adds one small static executable and a marker to desktop build outputs.
Existing profiles and the SQLite schema are unchanged. Moving or copying a direct
output requires keeping its marker and using the uninstaller from the copy being
removed; the tool cannot infer ownership after manual deletion of all artifacts.

## Evidence

The 2026-11-05 daily-uninstaller follow-up applies the same ownership rule to NSIS:
foreign startup values no longer block keep-history removal of the recognized
installed package. NSIS rechecks ownership before purge and removes only matching
Run/Owner values at cleanup time. Foreign ownership prevents history purge and
shared preview-container removal. A foreign installed-app location remains a hard
preflight failure. This does not merge the two uninstallers or make the development
tool responsible for installed package slots. [Verification](../../Tasks/UNINSTALL-daily.md)

[Native core/bootstrap fixtures](../../tests/integration/build_uninstall.cpp) exercise
keep/purge, startup off, foreign and changing ownership, registry denial, locked and
read-only files, NTFS junctions, cancellation and original-tool self-removal in
isolated registry namespaces and disposable Unicode directories.
[UI qualification](../../tests/compatibility/fluent/build-uninstall/README.md) records
the bilingual theme/DPI matrix, retry, pending, accessible Toggle and lifetime checks,
with physical acceptance limits kept explicit.
