# Screenshot-only hover previews

Status: Implemented; platform and release acceptance remains open where listed in the
[task record](../../Tasks/PREVIEW-screenshot-hover.md).

## Context

The accepted screenshot hover plan extends the card surfaces from plan sections 5–8
without publishing clipboard data or changing history. Decoding an untrusted full image
on the UI thread would increase both latency and the desktop process's attack surface.
Image-file references and text companions have different semantics and are excluded.

## Decision

The desktop owns one `win::ScreenshotHover`, shared by Workspace and Floating. Only
`CardMenuKind::screenshot` bodies qualify. Entry ID, snapshot ID, revision, source-window
generation and a monotonic request serial bind each candidate. The final UI callback
checks the actual pointer hit again. Hangers, controls, resize borders, text, files,
folders, groups, tray entries and menu summaries do not start this channel.

A 500 ms dwell starts an asynchronous request. The image fits completely without
upscaling, within 960 × 720 DIP, 80% of the monitor work area, and the pixel budget.
Right, left, below and above are tried in that order; constrained placement picks the
largest fitting result. A 12 DIP gap separates it from the rotated card bounds.
There is no preview if no non-overlapping area exists.

The single popup is layered, transparent to input, a tool window, and nonactivating.
`UpdateLayeredWindow` draws only when new pixels are ready. A 10 ms requested candidate/visibility
timer validates the hover, foreground window, buttons, capture and menus; it does not
repaint. Completion posts a message to the stable host listener so a warm image need
not wait for another timer tick. Card swing updates placement while preserving dwell
on the same identity. Surface gesture, hide, destroy, scroll, resize and navigation events cancel
immediately. Privacy cancellation destroys the window; ordinary leave reuses it and a
one-shot 30 second idle timer releases its compositor buffer. Screenshots suppress the
old body tooltip while the feature is enabled. Resolution labels remain independent.

## Data and security boundary

`ContentRepository::screenshot` checks the live identity and representation inventory,
selects the first saved PNG/DIB/DIBV5 in thumbnail inventory order, and acquires a Blob
lease. It never calls snapshot materialization or reads `screenshot_path`. Metadata and
the bounded inline representation are obtained under the store lock. Manifest/payload
hashing and cancellable chunked external reads run on the preview service thread after
unlocking. The lease protects concurrent history deletion/GC.

The service has one active request and one replaceable pending slot. Its CPU LRU is
independent of `ThumbnailCache`: 16 MiB, including the displayed shared image, at most
64 entries, keyed by Blob hash, encoding, target dimensions and decoder version. Failed
content is cached for 30 seconds. Sandbox launch/crash/timeout failures impose a global
30 second backoff. Once the cache expires the service sleeps until signaled. Ordinary thumbnail and outward-drag operations use their own supervised execution paths. [Worker roles](0026-preview-and-drag-workers.md)

The screenshot decoder mode of `ClipRopePreviewWorker.exe` runs with a capability-free AppContainer token and an atomic
Job assignment (one process, 256 MiB process commit, kill on Job close). A child-process
restriction is also applied. Only the preview executable receives a package-specific
read/execute ACE. No history, database or user-file directory is granted to this SID.
Windows-provided AppContainer access remains part of the operating-system boundary.
The environment contains only the Windows root and local application-data location
needed for profile resolution. No Shell, OLE, clipboard or network operation is exposed.

An explicit inheritance list contains one read-only input mapping and one bounded
output mapping restricted to `FILE_MAP_WRITE`. The protocol has magic/version, serial, encoding, dimensions,
stride and lengths. No payload path, database handle or parent-process handle crosses
the boundary. The parent validates header bounds without WIC; only the worker uses the
built-in PNG decoder or the checked DIB path. After worker exit, the parent validates
the response against the request and source header, then copies bounded PBGRA pixels.
The window uses a temporary GDI DIB; no additional GPU image cache is allocated. Windows'
layered-window composition allocation is separate from the 16 MiB CPU cache.

Input limits are 32 MiB, 8192 pixels per side and 8,388,608 pixels. Output limits are
2048 per side, 2,097,152 pixels and 8 MiB. The total request deadline is two seconds;
worker startup and decode receive at most 1.5 seconds of it. Cancellation stops pending
work and terminates the active Job, waiting for process exit before replacement. OS I/O
cancellation is drained before releasing its buffer. There is no in-process fallback.

Each uncached decode uses a fresh worker, which exits before the response is consumed.
This deliberately refines the proposed reusable child: immutable output after exit
avoids a validation/copy race, and decoder state is reclaimed immediately. Warm pixels
still reuse the CPU cache. The process handle supplies completion synchronization, so
an additional inherited event is unnecessary. Both refinements reduce shared state;
cold-launch cost remains subject to the same acceptance target.

## Settings and installation

`CardPreferences::screenshot_hover_preview` defaults to true. The optional strict table
`screenshot_preview_preferences(id, enabled)` has one constrained row. Missing table or
row means enabled; the core schema version stays unchanged. Cards saves it atomically
with existing preferences. A failed transaction retains the previous values. Reset
removes the optional row. Disabling immediately hides and purges; content mutations,
lock/session/display transitions and exit also invalidate transient state.

The installer requires the new sibling worker and moves it with each complete package
slot during install, upgrade and rollback. The profile is created lazily under the
fixed name `cliprope.screenshotpreview.v1`; a contained test parent derives a restricted
child SID when reopening it. Upgrades retain the identity. Uninstall removes the profile
using `DeleteAppContainerProfile` after idle/path validation and before removing
executable payloads, so a profile-cleanup failure retains the uninstaller. Capture-worker
permissions are unchanged. Direct portable copies retain the per-user profile until
their final installation is removed; it stores no application-generated previews.

## Alternatives and consequences

Extending the thumbnail cache or decoding on the desktop was rejected because it would
couple independent queues and increase the impact of a codec failure. Reusing the capture
worker was rejected because capture requires external clipboard access. No external
image-file preview behavior changes in this decision.

Failures hide the preview and retain ordinary card behavior. The security boundary
applies only to this new full-preview channel: existing thumbnail decoding is unchanged.
Diagnostics expose counts and failure classes, never image bytes, screenshot paths or
pixel logs. Tests use synthetic images and disposable histories.

## Evidence and references

`PREVIEW-01.sandbox`, `PREVIEW-02.desktop`, and `PREVIEW-03.faults` cover the data boundary,
three surface routes and adversarial responses. See the [task record](../../Tasks/PREVIEW-screenshot-hover.md)
for exact runs, performance scope and unverified acceptance cases.

Windows contracts: [layered-window hit testing](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features#layered-windows),
[AppContainer for legacy apps](https://learn.microsoft.com/en-us/windows/win32/secauthz/appcontainer-for-legacy-applications-),
[process attribute lists](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute),
and [profile deletion](https://learn.microsoft.com/en-us/windows/win32/api/userenv/nf-userenv-deleteappcontainerprofile).
