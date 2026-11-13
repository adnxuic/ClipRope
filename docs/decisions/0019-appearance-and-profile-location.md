# ADR 0019 · Appearance and profile location

Color controls and color persistence are superseded by [ADR 0023](0023-unified-color-picker.md). The descriptions below record the original implementation.

**Accepted · 2026-11-03.** Appearance exposes the theme and Find highlight color. Storage & Privacy shows the data folder and its disk usage, followed by the [capture exclusions editor](0020-general-settings.md). This extends the management surfaces in ADR 0016 and the selected occurrence in ADR 0017. [Management surfaces](0016-fluent-management.md) · [Find in Cards](0017-find-in-cards.md)

Appearance is an optional preference in the history database. Both new and older profiles default to Light and an active match background of `#E5A85E`. A committed theme applies to every open Fluent surface and subsequent openings. System observes Windows app personalization. High contrast takes precedence. The workspace uses the selected highlight on the active DirectWrite range in both views. Ordinary ranges retain `#F3DE9A`.

The normal desktop discovers its profile before opening a history writer. The per-user locator in `%LOCALAPPDATA%\ClipRope.Settings` starts with `%LOCALAPPDATA%\ClipRope` as the data folder. An explicit `--profile` bypasses the locator and exposes that folder's usage. The storage page measures allocated bytes for managed files in the active folder on a background thread. The scan includes backup stores, caches, managed screenshots, staging and database sidecars. External file references stay with their owners.

Selecting a folder records the pending destination. **Later** keeps the current session writing to its open store. **Restart now** saves the active paper edit, drains work and relaunches the executable after releasing the writer and locator locks. On startup the locator verifies both folder identities, locks the source and destination, checkpoints the source and stages managed files on the target volume. It verifies SHA-256 digests and database integrity before committing the active path. Moving a profile retains its local backups and materialized screenshots.

The durable transfer receipt records relative names, source digests and the identity of each file installed on the target. An interrupted installation can be rolled back before retrying with the latest source contents. A disconnected abandoned destination retains its cleanup receipt. After cutover, only verified source files are removed. A cleanup failure keeps the new store active and is retried on startup. Retaining history on uninstall preserves the locator for reinstallation. Explicit purge validates the active store and removes its data and location settings.

[Phase and verification](../../Tasks/SETTINGS-appearance-storage.md)
