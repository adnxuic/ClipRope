# ADR 0025 · One development runtime and a separate daily installation

**Accepted · 2026-11-04.** The contributor found many competing desktop outputs and
requested a complete build cleanup, one development executable, and separation of
the application used daily from production and qualification builds.

## Decision

Each checkout has one `build/cmake` tree and one `build/app/ClipRope.exe`. Debug,
qualification and production replace the same runtime serially. A PowerShell
workflow holds a checkout lock and supplies the CMake build session. Extra build
trees, renamed desktop baselines and hard-linked desktop images are rejected.
Helpers and test executables remain distinct. Tests that model multiple copies use
disposable Temp directories or an empty Windows test account, with cleanup.

The daily executable is in the existing per-user installation's registered `current`
slot. First installs default to LocalAppData/Programs/ClipRope and can choose a
different folder. Upgrades and rollbacks reuse the recorded root. Build, Test and
CleanRebuild do not install anything.
Publish is an explicit operation and requires matching source, runtime, configuration,
cache and successful regression records. Its test selection remains visible; the
local core gate does not claim full Office, display or performance qualification.
The installer's existing previous-slot rollback remains outside the build tree.

An uninstalled desktop must supply `--profile`, or a qualification-only isolated
local-app-data root. A package marker alone is insufficient to select real history:
the registered installation must identify the current executable. Rejection happens
before profile storage or startup initialization, returns exit code 2 and writes a
debug diagnostic. No new application window or visual component is introduced.
RunDev supplies a disposable profile and checks for the daily process first.

Private-window-station fixtures isolate automated clipboard work. Physical input,
tray and Office tests are opt-in and require the daily process to be stopped or an
independent Windows test session. Tests never silently fall back to the daily clipboard.

## Cleanup and failure

CleanRebuild verifies a compressed archive of the complete old build, checks the
resolved target, reparse points, running images and file access, then removes only
that build. Archives stay under ignored `.local-artifacts`; real history, location
settings and user edits remain outside cleanup. Historical evidence keeps its
original paths and is labeled as archived rather than rewritten.

A build removes its success receipt before mutation. Variant switches remove the
small executable output set to force relinking; object caches remain reusable.
Test restores Production Release even after a test failure but preserves failure
in the test receipt. If rebuilding fails, an available compressed last-production
payload is restored without certifying it for changed sources. A failed initial
migration remains recoverable from the cleanup archive; it never updates ownership
before a successful installation. Publish refuses occupied applications instead of
killing them, and preserves the existing sign-in choice.

This supersedes the multiple-output examples and direct-build daily startup in
earlier guides. ADR 0024's guarded local uninstaller still accompanies development
builds; it does not replace build-directory cleanup or the installed uninstaller.
No history schema changes are required.

## Evidence

See [build workflow qualification](../../Tasks/BUILD-single-runtime.md) for commands,
backup inventory, actual migration results and remaining acceptance work.
