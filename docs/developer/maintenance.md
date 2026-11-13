# Maintaining ClipRope

Use the stage gates to decide when an implementation can advance. The [task ledger](../../Tasks/README.md) tracks verified phases and the single-display qualification environment. The [implementation plan, sections 15–17](../../ClipRope_Long_Term_Implementation_Plan.md) defines the sequence. The [test guide](testing.md) specifies the evidence to collect.

## Milestones and exit evidence

| Gate | Deliver and demonstrate |
| --- | --- |
| P0 · Windows probes | Demonstrate cross-process click-through at the reference display scale and verify self-write detection. On card double-click, prove publication followed by one insertion into the previously selected editable text box. Repeat with no editable target and verify manual Ctrl+V. Exercise a changed target and unconfirmed delivery. Record offered versus saved/lost formats for at least one Office object. |
| P1 · Data core | Without a UI, verify that external blobs are in place before the SQLite transaction commits admission and tail trimming. Prove unified order, rope-free Floating persistence, version rejection, restart, multi-format text round trip, listener loop suppression and rollback. The model and commands must already accommodate multiple ropes and representations. |
| P2 · One-rope workspace | Demonstrate an external copy through commit, display, Hanging double-click, promotion, workspace collapse, capacity overflow and restart. Verify the selected-field and manual-paste branches through the actual card. Rope-head and tray invocation publish for manual paste even with a text box selected. |
| P3 · Floating and editing | Detach, restore and close a Floating card using each deletion mode. Double-click opens or edits the Floating content and preserves the full rope, clipboard and workspace visibility. The return button changes membership and order only. Verify partial failure outcomes from the test guide and check that styling leaves content intact. |
| P4 · Multiple ropes | Exercise the main rope, creation/switch/deletion, cross-rope moves and the virtualized overview. Test three full ropes, deletion of a former rope, shared Blob safety and delayed commands crossing a rope switch. |
| P5 · Format compatibility | Validate file groups, image pixels, RTF/HTML and supported virtual formats. Confirm ordinary native paste while observing and match capability labels to actual restoration results. Archive real Office matrix rows. |
| P6 · Release candidate | Exercise external-capture isolation, all-process performance budgets, stress, multi-display and permission cases, fault injection, backup and restore, diagnostics, privacy controls and packaging. Record failures and affected environments. |

## Changes to architecture and contracts

1. Identify the affected command, owner and visible behavior in [architecture](architecture.md), [algorithms](algorithms.md) and the [data model](data-model.md). Product rules come from the revised plan. Keep unmeasured engineering proposals labeled as such until a probe or decision resolves them.
2. For a consequential choice, write a short English ADR in `docs/decisions/`. Include status, context, decision, alternatives, consequences, relevant plan sections, affected persisted data and evidence. Examples include capture-worker isolation, transparent input routing, Blob installation/commit order, unified eviction order, deduplication and plain-text edit downgrade. Link a superseding decision to its predecessor.
3. If bytes, manifest fields, SQLite constraints, or supported representations change, update `docs/schemas/`, [format conventions](formats.md), version checks and tests. A change touching a clipboard side effect or ownership rule must update the command contract, acceptance cases and affected UI wording together.
4. Attach the run configuration, results and remaining unknowns to the change. Update `docs/compatibility-matrix/` for adapter/UI claims and keep comparisons tied to a concrete app/version/path. Use the reference workload and full process set when recording a new performance baseline.

## Preserving user history

Fully exit ClipRope before using its offline recovery commands. Keep the
original directory intact, including any WAL, shared-memory file and blob
directories. The headless executable opens the source database read-only.
For example, from the folder containing the installed binaries, substitute
your own existing profile, a new backup path and an empty recovery path.

```powershell
.\cliprope_headless.exe "C:\History\Profile" verify
.\cliprope_headless.exe "C:\History\Profile" backup "C:\Backups\Profile-1"
.\cliprope_headless.exe "C:\Backups\Profile-1" restore "C:\History\Recovered"
.\cliprope_headless.exe "C:\History\Recovered" verify
.\ClipRope.exe --profile "C:\History\Recovered"
```

Resolve an access failure by checking the Windows account's read/write
permissions. Free space before retrying a full drive. If one blob is missing
or its digest fails, preserve the affected entry and the profile, and check a
known-good backup. If SQLite reports damage, copy the closed profile as a
whole for investigation and restore the verified backup into a separate
folder. The recovery tool refuses to replace a populated destination. Recovery accepts verified ClipRope v11 profiles and backups.

For a complete backup, snapshot the SQLite database with the online-backup API or another explicitly verified equivalent. Enumerate all externally stored Blobs referenced by that snapshot, including Floating-only content, and coordinate leases with GC for the entire copy. Compare sizes and hashes to the manifest and test restoration into a fresh directory. Copying a live database file in isolation can omit committed WAL data and external files.

A version refusal preserves the source history. Use a fresh directory to start a new history. [Current format policy](../decisions/0028-current-storage-formats.md)

Review export, GC, clear and uninstall paths together when ownership changes. Clear-current-rope and clear-all-ropes affect Hanging only. An explicit delete-all-data action includes Floating and requires confirmation. Deleting a file-reference card releases ClipRope's reference to the user's file. Erasing application history does not establish forensic erasure from SSDs, WALs or backups.

## Release review

- Rebuild using the pinned toolchain and dependencies. Run domain and storage tests for each commit once available. Archive reproducible Windows graphics/clipboard scenarios, Office compatibility and Release x64 performance results. Review static analysis and resource release paths for COM, GDI, native handles and storage media.
- Review diagnostics using synthetic content. Logs may include status, error code, format name and timings. Exclude clipboard bodies, HTML, file content and full paths, including from exported logs. Diagnostic export is a deliberate user action. The baseline operates locally with network use off by default. Verify pause, application exclusions, history-exclusion markers and deletion scopes.
- ClipRope's original code and accompanying documentation use the [MIT License](../../LICENSE). Keep the full copyright and permission notice in the [installer notices](../../packaging/windows/ThirdPartyNotices.txt). Review artwork and bundled dependencies under their respective terms when packaging a release.
- Prefer per-user installation and normal privileges. New installations enable per-user sign-in by default. Upgrades and rollbacks retain the current choice. [General settings](../decisions/0020-general-settings.md) Ask whether to keep history during uninstall. Record install, upgrade, rollback and uninstall outcomes against actual packaged builds. Classify each platform/version as tested, failed or untested.

## Install, roll back and remove a candidate

Exit ClipRope through the tray and run setup as the current user. Select the desired sign-in setting. On a first installation, accept `%LOCALAPPDATA%\Programs\ClipRope` or browse to an empty or new writable folder on a local drive. Keep the program separate from history and settings. The executable and recovery tool live in the chosen folder's `current` subdirectory. Each installation writes its source revision and version to `release.ini`. The account's Start menu, Apps registration and optional Run entry use that current slot. History defaults to `%LOCALAPPDATA%\ClipRope`. Settings → Storage & Privacy shows the active folder and lets the user schedule a verified move. [Profile location](../decisions/0019-appearance-and-profile-location.md)

For unattended installation, append the final argument `/D=D:\My Apps\ClipRope` without quotes. Upgrades and rollbacks always use the registered program folder. To change drives, uninstall, choose to keep history, then run setup again and select the new folder.

An upgrade prepares a complete incoming directory, preserves the previous executable set, then renames the incoming slot into place. Run the same installer with `/ROLLBACK` to switch back. Use a backup supported by the selected build when restoring its history.

Interactive uninstall asks whether to keep history and local backups. A silent uninstall keeps them by default. `/S /PURGEHISTORY` requests deletion of the recognized product profile, including Floating entries and local backups. The cleanup helper refuses linked folders, non-ClipRope databases, an active writer and unexpected top-level files. It resolves the active folder through the per-user locator. Keeping history also keeps this locator.

For a CMake output, use its sibling `Uninstall.exe` after exiting the tray app. This
native tool uses `ClipRope.build.ini` and has an interactive keep/delete choice;
the NSIS switches above apply only to installed versions. It removes recognized
runtime filenames and startup values owned by that exact path, preserving other
build files and other copies' registrations. It does not create an Apps entry.
The shared history/locator stays unless deletion is explicitly selected and its
guards pass. Independent `--profile` directories stay in place. One reusable
verified helper remains under `%TEMP%\ClipRope.Uninstall` so the source uninstaller
can remove itself. Rebuilding the desktop restores the tool and layout marker.
[Direct-build contract and failure boundaries](../decisions/0024-direct-build-uninstall.md)

The [rehearsal](../../tests/compatibility/install_rehearsal.ps1) requires a normal user with no existing ClipRope installation, history, shortcut or sign-in entry. It exercises a distinct P5 predecessor, the P6 candidate, an actual binary rollback and both uninstall choices. The native [installed-history fixture](../../tests/compatibility/installed_history.cpp) launches the executable with its default profile, verifies its process token, captures a real external copy and reads a restored clipboard publication. Its receipt compares entry IDs, snapshot IDs and payload hashes across versions.
