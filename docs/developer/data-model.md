# Data model and persistence

The product rules below apply to every storage implementation. The [versioned SQLite schema](../schemas/history.md) and [content format](../schemas/content.md) describe the implemented P1 store. The original design covers ownership in sections 2–4 and persistence in section 10. [Implementation plan](../../ClipRope_Long_Term_Implementation_Plan.md)

## Identity and ownership

| Object | Meaning and lifetime |
| --- | --- |
| Rope | Owns an ordered collection of Hanging entries. Ordinary ropes have a positive capacity. The second protected system rope, ReBin, has unlimited capacity. Exactly one main rope exists, is permanently named `main`, and survives all rope deletion commands. Each rope has its own display settings and viewport anchor. |
| Entry | A card with a stable `EntryId`, one snapshot reference and its own presentation. Moving, detaching, reordering and reattaching preserve its identity. Copying to another rope creates a new identity. |
| ContentSnapshot | Immutable description of a single capture or content edit. Contains a format manifest, per-format outcomes, capture time and a source hint. Multiple entries can share a snapshot. |
| Representation | Describes one format of one snapshot. A successfully saved representation identifies the bytes and the metadata needed to reconstruct that format. A failed or skipped format can describe its outcome without claiming to have recoverable bytes. |
| Blob | Immutable bytes addressed by strong content hash and byte length. Identical bytes can back multiple representations. A thumbnail or a matching text preview is insufficient evidence of identical content. |

Read the relationships from left to right. One rope has zero or more Hanging entries. Many entries may reference one snapshot. A recoverable snapshot has one or more saved representations and may also list skipped or failed formats. Each saved representation resolves to one blob. Several representations may reuse the same blob. Removing one entry releases its reference to this graph.

Each committed entry has exactly one of these shapes.

| State | `rope_id` | `sort_key` | Number and capacity |
| --- | --- | --- | --- |
| `Hanging` | References one existing rope | Present | Numbered from 1 inside that rope. Counts toward that rope's limit. |
| `Floating` | NULL | NULL | Has no rope number. Survives every rope's overflow, shrink or deletion and persists across process restarts. |

Detaching keeps the entry and its snapshot, saves its desktop layout and releases one rope slot. Closing a Floating card moves the entry to ReBin by default. Direct deletion releases that entry's reference to its snapshot. [ReBin decision](../decisions/0014-rebin.md) Window destruction during exit only ends its on-screen lifetime. The Floating collection needs neither an origin-rope foreign key nor a protection flag. The decorative hanger has no domain identity.

For each finite rope, the number of committed Hanging entries is at most its capacity. The total number of cards is the sum of those counts plus the independent Floating count. A default capacity initializes a new rope. It supplies no global card limit. The active collection rope exists even while the workspace is hidden.

## Logical record map

The application exchanges stable IDs and value records with the domain. The persistence adapter maps them to the tables below. The schema SQL defines defaults, constraints, timestamp units and stable ID storage. [History schema](../schemas/history.md)

| Table | Fields and relationships |
| --- | --- |
| `ropes` | `id`, name, color, `sort_rank` for rope display order, `capacity`, `is_main`, creation time. |
| `entries` | `id`, required `snapshot_id`, required `state`, nullable `rope_id` and `sort_key`. Track a revision for stale edit and async result detection. |
| `snapshots` | `id`, `captured_at`, `source_hint`, `preview_kind`, `capture_status`, `manifest_hash`. The source is a hint for filtering. Capture time is informational. |
| `representations` | `snapshot_id`, standard format ID or registered format **name**, medium (`tymed`), aspect, `lindex`, `codec_version`, priority, result, byte count and `blob_id` when bytes were saved. Resolve a registered format name to its current Windows ID during publication. |
| `blobs` | `id`, hash, size, storage kind and exactly one payload location for saved content (inline bytes or a store-relative path). Uniqueness on the content key permits byte reuse. |
| `card_presentation` | Keyed by `entry_id`. Whole-card font size, weight, text and background colors, opacity, per-entry width and height scale, and Floating layout. Store monitor information, dimensions in DIPs and position relative to the work area. |
| `rope_view_state` | Keyed by `rope_id`. Stable anchor entry ID, horizontal offset, view version, single-rope top offset in DIPs (default 24), and compact density (default false). Density changes save the flag and rescaled anchor together. Removing or moving the anchored card can invalidate the horizontal anchor. |
| `app_settings` | Active rope, workspace mode, new-rope capacity, five card preferences. A per-entry flag controls the font override. Unmarked entries resolve their text size from the global setting when read. Switching to the main rope on deletion of the active rope keeps subsequent captures routable. |
| `clipboard_follow_preferences` | Optional singleton holding the independent card-head and collector switches. A missing row enables both. Global reset removes the saved row. |
| `card_delete_preferences` | Optional singleton with independent Hanging and Floating recycle flags. A missing table or row uses direct deletion for Hanging and recycling for Floating. Global reset removes the row in the settings transaction. |
| `general_preferences` | Optional singleton for workspace topmost and a pending rollback of sign-in registration. A missing row enables workspace topmost. [General settings](../decisions/0020-general-settings.md) |
| `localization_preferences` | Optional singleton containing `auto`, `en-US` or `zh-CN`. Missing/unknown values select Auto. Global reset deletes the row atomically. No core schema version change. [Localization](../decisions/0021-bilingual-localization.md) |
| `shortcut_preferences` | One optional override per shortcut action, containing its enabled flag and chord. Missing actions use their built-in defaults. Windows registration status belongs to the current session. |
| `schema_migrations` | Schema version and creation timestamp. A fresh v11 database records version 11 in the creation transaction. |

Representations of a snapshot describe what was actually preserved. File references contain an ordered list of user paths. A group of paths consumes one entry. Virtual-file streams, image pixels, static file references and display thumbnails have distinct meanings. Editing card style writes presentation only. An edit to the content creates a new snapshot and switches only that entry's reference. [Algorithm contracts](algorithms.md)

### Color ownership

`domain::ColorSelection` distinguishes Default from explicit RGBA. The persistence
adapter serializes it as `default` or `#RRGGBBAA`; renderers resolve semantic defaults
at the scene boundary. `RopeMaterial` owns the five shades and deterministic fiber
seed for each rope. See the [color schema](../schemas/history.md#explicit-colors-and-materials).

Only the UI thread owns the temporary card/rope color overrides. They bind stable
target identities and do not modify Browser summaries or SQLite. Commit validates
the original field in the same transaction as the recent-color update. A staged
rope property color belongs to its outer form until Save or Cancel.

### Query and constraint rules

New and renamed ropes accept nonblank Unicode names of at most 256 UTF-16 code units. A supplementary character uses two units. Embedded NUL and malformed UTF-8 are rejected. The same rule applies to the native dialog, inline property popup and public store command. Reads validate saved names against the same rules. The reserved main and ReBin names remain bound to their system identities.

The saved Floating visibility flag determines whether startup materializes a window. A hidden entry keeps its snapshot and monitor-relative geometry. When hidden entries exist, the tray offers **Show a hidden Floating card**. Opening that menu binds one hidden ID in stable ID order, starting after the previous attempt and wrapping at the end. Selecting the row prepares the HWND, commits the resolved visible placement and then shows the window. A failed card remains available on the next pass, and its peers can still be shown.

- Enforce state as non-null and closed to the two values. Use a check equivalent to the expression below, a rope foreign key, an entry-to-snapshot foreign key and a positive capacity check for finite ropes. Only the reserved ReBin ID permits a NULL capacity. Route rope deletion through the application transaction. A unique index can enforce *at most* one main rope. Initialization establishes *at least* one.
- Index Hanging rows by `(rope_id, sort_key, id)`. Smaller keys sort nearer the head. Use the same ordering for display, paging and tail selection, with stable ID as the deterministic tie-breaker. Derive the one-based number at read time. Capture time, command sequence and Floating management order play no role in eviction.
- Query Floating directly by state on restore. All persistent entries contribute to reachability regardless of rope ownership. A query that starts at `ropes` cannot enumerate the whole history.
- Page Hanging summaries within the viewport. Carry a rope ordering version with each cursor and invalidate stale pages after mutations. Return async previews with entry ID and content version so recycled UI slots can discard stale results.

```sql
CHECK (
    (state = 'Hanging' AND rope_id IS NOT NULL AND sort_key IS NOT NULL)
 OR (state = 'Floating' AND rope_id IS NULL AND sort_key IS NULL)
)
```

Sparse integer sort keys are a candidate for cheap head insertion and drag reordering. If a gap is exhausted, reassign keys in the same transaction and preserve the logical order. Storage tests must cover collisions against the chosen uniqueness constraint, including during bulk reassignment.

## Local store and commit protocol

```text
%LOCALAPPDATA%\ClipRope\
  history.db
  history.db-wal          SQLite-managed when present
  history.db-shm          SQLite-managed when present
  blobs\xx\<hash>.blob    large immutable payloads
  thumbnails\             rebuildable previews
  staging\                controlled uncommitted files
  logs\                   diagnostics without captured content or full paths
```

Keep the live database on local storage. Start validation with foreign keys enabled on each connection, WAL and `synchronous=FULL`. Measure the inline/file threshold with real content. Process termination tests and physical power-loss tests exercise different guarantees.

1. Serialize and validate the selected content. Stage external bytes on the blob volume, verify size and hash, and keep in-flight data protected from collection.
2. Perform the required flush and promote the completed file by same-volume rename. If identical content already exists, reuse its immutable backing. Every referenced file must be ready before the database can commit its reference.
3. On the storage writer, commit snapshot, representations, blob references, entry, rope order and the target's overflow removals in one SQLite transaction. Inline bytes participate in this transaction. On error roll back all database changes, including removals.
4. After commit, publish one change set to the view and mark the capture saved. A crash between file placement and database commit may leave an orphan for later cleanup.
5. Garbage collection may reclaim bytes only after they are unreachable from **all** committed entries and free of runtime leases. Coordinate lease acquisition, commit and reclamation so collection cannot race a new reference or an active reader. Leases cover published clipboard data, external drags, exports or backups, and in-flight commits. Deleting a file-reference card never deletes the user's file.

Collection commits orphaned SQL rows, snapshots the retained Blob identities and releases the primary database mutex before walking the profile directories. The history store serializes collection with capture file installation. The collector rechecks an orphan's current row and leases before removal. A derivative deletion claims the SQLite writer against the separately supervised thumbnail installer. A locked file can be retried after release. Foreign cache names are outside the shared thumbnail grammar. [Matched reader/collector observations](../../Tasks/architecture-evidence/AR2-D.md)

High-frequency position and style changes can coalesce. Flush the final value when a gesture or edit finishes. Capture and eviction commit at operation time. On recovery, reconcile staging and orphan files. Surface a missing referenced blob or a damaged database as damage and keep the evidence available.

## Search fields

The index keys each derived field by snapshot ID and ordinal. The original field text preserves UTF-16 range mapping for rendering. The search joins each field to live Hanging entries, which supplies current rope ownership and order. Editing one of two shared entries installs fields for its new snapshot. Snapshot deletion cascades to its fields. Queries and selected occurrences live only in the search session. [Find in Cards](../decisions/0017-find-in-cards.md)

## Backup and recovery

Take a consistent database snapshot plus a manifest of every external blob reachable from it, including Floating-only content. Coordinate the snapshot with blob leases and hold them until copying finishes. A database-only backup omits file-backed content.

The desktop and recovery tools check the application ID and schema version before using a profile. A backup must contain a valid manifest for every reachable snapshot. [Verification cases](testing.md)
