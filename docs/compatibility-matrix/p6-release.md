# Windows x64 release qualification

| Artifact | Identifier |
| --- | --- |
| Source revision | `b7d60fd8b08db5f5a8f463dac3fc1223674fc652` |
| Installer | `ClipRope-0.1.0-rc.1-windows-x64.exe`, 2,230,274 bytes |
| Installer SHA-256 | `b4b5314f4b53ac96ce0fb4160e19e27492df7685fdc9be4ac3027f20aa64a383` |
| ClipRope.exe SHA-256 | `b4e8d968f7cec2489c1caa3e41c0509d998788dbcb305f6743853cf1d8aec588` |
| ClipRopeWorker.exe SHA-256 | `f760e93140046a162b704499a96939e08f3df4b6445dfeebed06181ea058f29f` |
| cliprope_headless.exe SHA-256 | `003d6ba0219517928fa7b89a4f56aea618a3b122839735b34e0cd0651ff5fca2` |

This candidate targets the local Windows 11 25H2 x64 console on build 26200.9711, one 1920 × 1080 monitor at 125%, a standard user and the tested Win32 Edit controls. The [desktop matrix](p6-desktop-2026-11-01.md) separates physical input, actual process integrity boundaries and system-notification replay. The [Office matrix](p5-office-2026-11-01.md) gives the application builds, paste targets and editing losses for all eight objects.

## Runtime and recovery

The resident application launches a short-lived capture process for external objects. The worker receives neither a writable database nor a history directory. The tested deadline, bounded backlog, host responsiveness, reclamation and process-tree comparison are in the [capture decision](../decisions/0002-external-capture-worker.md). A failed or expired read cannot replace a newer card or enter another rope.

On the reference desktop a stable Win32 Unicode copy occasionally received `E_OUTOFMEMORY` from the system OLE format enumerator. The same provider, version and source bytes were still present. Its [failure receipt](../../tests/performance/results/ole-enumerator-failure/receipt.txt) distinguishes this case from a separate, reproduced 266 ms clipboard-reader lock. The worker can recover strictly verified native Unicode, and can reacquire an OLE offer within the original ticket when the first enumeration is unusable. Reacquisition repeats both privacy checks before reading a payload. Unknown formats and indeterminate privacy responses do not authorize native recovery. [Adapter rules](../schemas/clipboard-adapters.md)

The [storage qualification](../schemas/history.md) distinguishes process termination at five boundaries from physical power loss. It covers SQLite disk-full, a real directory ACL denial, missing content, database corruption, concurrent export with GC and v1 migration rollback. Diagnostics identify the recovery action. The original damaged data and known-good backups remain available.

The [privacy qualification](../schemas/capture-privacy.md) includes the ordinary Settings controls, local-history and cloud-only markers, the three deletion scopes and an explicit diagnostic export. In the [network-isolated process test](../../tests/integration/offline.cpp), the production capture and recall programs completed the Unicode/HTML loop with Winsock blocked for the test process trees.

## Distribution inventory

| Item | Shipped material and provenance |
| --- | --- |
| First-party application | ClipRope desktop executable, capture worker, local recovery utility, install and uninstall routines. The rope, papers and controls are drawn in code. The repository's design-reference PNG, original brief and synthetic Office test documents are excluded from the installer. No third-party ClipRope/CopyQ code or asset is imported. No public open-source license was selected for the original work. |
| SQLite | Unmodified 3.54.1 amalgamation, compiled into the relevant executables. Archive SHA3-256 and upstream public-domain dedication are in the [source notice](../../third_party/sqlite/README.md). |
| Microsoft toolchain and Windows | Visual Studio 2022, CMake 3.28.3-msvc11, MSVC 19.40.33811, Windows SDK 10.0.22621.0. Release programs use the static multithreaded C++ runtime. Windows supplies native APIs, Shell, Direct2D, DirectWrite, WIC and fonts. No Office installation, font file or Windows component is redistributed. [Microsoft runtime deployment](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170) |
| Installer | NSIS 3.13 Unicode stub, System and nsExec plug-ins, zlib compression. The build verifies the portable compiler archive's SHA-256. Users receive the [notices](../../packaging/windows/ThirdPartyNotices.txt) with the application. [Upstream NSIS license](https://nsis.sourceforge.io/License) |

The local evaluation installer is unsigned. Per-user installation and the resident process both use normal user privileges. Login startup requires an opt-in. The actual upgrade rehearsal uses a production build of the P5 closing revision `0bbbe44` as the predecessor.

## Build and installation checks

The shipping preset compiled the desktop, worker and recovery tool with `/W4`. The first MSVC static-analysis pass identified oversized stack temporaries, a null-handle path, an unchecked COM wait result and an entry-point annotation. After corrections, `/analyze` completed without warnings on the first-party release targets, including the packaged cleanup helper. The pinned upstream SQLite amalgamation was excluded from first-party analysis. `dumpbin /dependents` found only Windows libraries in all three application executables. The worker uses the GUI subsystem and starts without a separate console process.

The complete qualification preset discovered and passed **58/58 cases in 130.41 seconds**. A separate `git archive` of the identified revision was expanded into an empty source directory and built into a new binary tree with the pinned Release x64 toolchain and bundled SQLite. An independent source then copied into that build's hidden listener. Reopening it restored the same collector, entry ID, snapshot ID, Unicode and HTML. [Registered case names, timings and results](p6-ctest-2026-11-01.txt)

The packaged desktop and worker completed the 900-round stress test and the full performance workload. All 1,173 expected byte hashes were admitted on the correct ropes. Four observed enumeration failures recovered within the original ticket. All ten budget checks passed. App-side scrolling had an 11.34 ms p95 and 13.16 ms p99. Including the compositor fence, 22 of 240 deadlines were exceeded on the 165 Hz reference display. [Final performance results and raw trials](../../tests/performance/qualification.md)

The installer rehearsal began on the reference standard user's existing Windows account with a fresh ClipRope profile. It checked that the default data and program folders, Apps registration, Start menu shortcut and sign-in value were absent before making changes. The installed resident reported a non-elevated process token. Synthetic copies and paste reads ran on the test window station. [Rehearsal driver](../../tests/compatibility/install_rehearsal.ps1) · [Installed history verifier](../../tests/compatibility/installed_history.cpp)

| Operation | Observed result |
| --- | --- |
| Install the P5 predecessor | A new external copy entered the default profile. The recovered Unicode and HTML matched. The fixture recorded Hanging and Floating identities, snapshot IDs and representation hashes. Sign-in startup was off. |
| Upgrade and opt in to sign-in startup | The registered version and executable hash changed. The new sibling capture worker admitted and restored a fresh external copy. The default history fingerprint matched and the current user's Run value identified the installed executable. |
| Reject an unsafe or concurrent package change | A held setup mutex, an unknown file in the previous slot and a locked current executable each stopped the attempted package change. Both version directories survived. |
| Roll back | The previous-version executable hash, retained sign-in preference and default history fingerprint matched their receipts. The predecessor read and published the saved card. |
| Retain and reinstall | The uninstall removed binaries, registration, shortcut and login startup. Reinstallation reopened the same Hanging and Floating history with intact hashes. |
| Explicitly remove history | The verified, fixture-owned profile and application files were removed. Registry and shortcut checks returned the user environment to its preflight state. |

The final rehearsal compared the revision in the installed `release.ini` and the SHA-256 of the installed `ClipRope.exe` with the release identifiers above. The earlier version had a separate executable hash. [Installation receipts](p6-install-2026-11-01.txt)
