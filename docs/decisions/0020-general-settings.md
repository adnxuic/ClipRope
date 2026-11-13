# ADR 0020 · General settings

**Accepted · 2026-11-03; updated 2026-11-09.** General groups Language and Always on top under Application. Startup contains Start with Windows. Advanced contains Reset settings…. Language offers Auto, English and 简体中文, saves immediately and refreshes open application UI. The two switches apply immediately. [Settings Center](0016-fluent-management.md) · [Language selection and refresh](0021-bilingual-localization.md)

The normal desktop and installer share the current user's 64-bit `Software\Microsoft\Windows\CurrentVersion\Run` value named `ClipRope`. Its command contains the quoted absolute executable path. The owner path in `Software\ClipRope\Startup` distinguishes an initialized direct run with startup disabled from a new user. An existing installation without the marker inherits its current Run choice. New installations and first direct runs enable sign-in. Setup can take over a known direct run, preserving its choice through upgrades and rollbacks. An explicit `--profile` instance isolates the system registration from both its switch and its reset.

A conflicting owner reports the other executable explicitly and preserves that
copy's registration. A direct-build uninstaller removes the Run and Owner values
only while they still identify its own executable, including the startup-disabled
case. [Direct-build removal](0024-direct-build-uninstall.md)

The main workspace defaults to topmost. Creation reads the preference, and a change applies without activating or moving the window. Reopening, switching views and changing DPI preserve the selected stacking mode. Floating windows retain their independent stacking behavior.

**Storage & Privacy → Privacy → Clipboard capture exclusions → Edit list…** opens a list of saved executable basenames. Add accepts a filename such as `KeePass.exe` and compares names without regard to case. Add, Enter in the name field and Remove edit the draft. Save commits the list, preserves the current capture pause flag and applies the new policy to subsequent clipboard events. Esc and Close discard the draft. A concurrent edit or failed write leaves the editor open.

The desktop always uses the standard image size and disables the transparency checkerboard. Older image preference records remain in the profile. The Cards controls for file previews and source resolution are independent. [Image card geometry](0009-adaptive-image-cards.md)

Reset opens an owned confirmation with Cancel selected. It restores the workspace preference, the five global card settings, new-rope capacity, five shortcut bindings, appearance and Auto language selection. The confirmation explains the language reset. A normal desktop also enables sign-in. The reset writes only these preferences in one transaction. Entries, snapshots, individual overrides, rope state, data paths and moves, and capture policy retain their values.

Before reset temporarily enables an absent Run entry, the host commits a rollback receipt. The SQLite transaction writes the defaults and clears this receipt together. A failed or interrupted reset with an outstanding receipt restores the former disabled registration before another sign-in change. After commit, the host reapplies language, theme, card rendering and runtime shortcuts. The Shortcuts page reports availability when another application owns a default chord.
