# History database v11

`history.db` is a UTF-8 SQLite database with application ID 1129467973,
`user_version=11` and a matching row in `schema_migrations`. Initial setup
creates one main rope with ID and immutable name `main` and capacity 100, plus the unlimited system rope `ReBin` with ID `system:rebin`. Each writable connection
enables foreign keys, WAL and FULL synchronization. An exclusive Win32 file
handle on `.writer.lock` protects the profile, including staging, external
files and collection. Windows releases this lock when the process terminates.
All operations on the live connection and its external data share one writer
mutex. The [v11 schema](../../src/persistence/schema_v11.sql) creates the tables, indices, triggers and both system ropes in one transaction. A fresh profile installs the committed file by an atomic rename. Interrupted creation can retry from the empty profile.

| Records | Persistent contract |
| --- | --- |
| Ropes, settings and view state | Each rope has a stable text ID, name, color, sort rank and a positive capacity or SQL NULL for ReBin. Both system ropes survive deletion. Settings reference the last ordinary collector. Connection-local ReBin visibility and selection reset on restart. A viewport has an anchor ID, horizontal offset, version, single-rope top offset in DIPs and a compact flag constrained to 0 or 1 (default 0). A density change saves its flag and anchor together. Deleting its anchor sets the reference to NULL. |
| Entries and presentation | A Hanging entry has both a rope and integer sort key. A Floating entry has neither. A CHECK plus insert/update triggers enforce these shapes. The entry owns its revision, font override, bold flag, colors, opacity, width and height scales (each 0.75 through 2, default 1) and independent Floating monitor/rectangle in DIPs. Moving an entry preserves its ID and snapshot. |
| Snapshots and representations | Capture time uses Unix seconds. A snapshot stores its source hint, capture status and manifest hash. Its ordered representations specify a standard numeric ID or registered name, medium, aspect, index, codec version, priority and outcome. Saved rows reference one blob. Skips and failures carry no blob reference. |
| Blobs | The ID is the 64 lowercase hexadecimal digits of SHA-256. Size is in bytes. A row stores either an inline BLOB or the relative path `blobs/xx/<id>.blob`, where `xx` is the first two digits. Recovery verifies the bytes before restoration. |

The settings row holds the Hanging double-click mode, inherited font size (6–96 DIP), resolution caption, single-image-file thumbnails and group retention. The four switches default on. The initial font is 14 DIP. A color or emphasis edit preserves the entry’s current inheritance.

For one rope, query Hanging rows in ascending `(sort_key,id)` order. The
`hanging_order` partial index serves that query. The head has the smallest
key. Entry identities break ties deterministically. List Floating rows
directly from `entries`. Snapshot reachability follows both states.

The optional singleton `clipboard_follow_preferences` stores `card_head` and `collector`
as checked 0/1 values. A missing table or row enables both switches. Saving either
preference commits both values atomically, and global reset removes the row.
[Collector head clipboard](../decisions/0024-collector-head-clipboard.md)

## Explicit colors and materials

`card_presentation.text_ink`, `card_presentation.paper_tint`,
`appearance_preferences.match_value` and `ropes.color_value` contain either the
literal `default` or canonical `#RRGGBBAA`. Default is a semantic selection resolved
by the corresponding style. It is never inferred by comparing RGB bytes. Ink,
Find and rope adapters require opaque alpha. Paper alpha is tint strength over
`#FBF7EF`; whole-card opacity is a separate field.

Each rope stores its natural or tinted material, five tones and texture seed alongside its selected color.

`recent_colors(color,rank)` stores at most eight distinct committed custom RGBA
values, newest first. A color commit and its recent record share one transaction.
Preview, failed writes, semantic Default and staging in rope properties do not
insert recents. Global reset clears recents and restores the Find default; card
and rope content remain intact. Copies and backup/restore retain selection identity
and material data.

Card color writes validate the entry ID, snapshot/revision and original target
field, then update that field alone. They leave text revision, age, membership,
ordering and other presentation fields unchanged. Rope color writes similarly
guard the original selection.
[Color decision](../decisions/0023-unified-color-picker.md)

## Backups and format checks

The optional `localization_preferences` table contains at most one row (`id=1`) and
a stable `language` value: `auto`, `en-US` or `zh-CN`. Missing tables/rows and unknown
language values mean Auto. The first save creates the STRICT table transactionally.
Global reset removes its row in the same transaction as the other global preferences;
a failed save/reset retains the former language. This optional table does not advance
core history schema and is included in ordinary backup/restore. Translation labels
are never stored in history snapshots or search fields.
[Language contract](../decisions/0021-bilingual-localization.md)

`Database::backup_to` requires a new destination directory. While holding the
profile writer mutex, it opens an independent read-only WAL transaction, fixes
its view of the database, enumerates external blobs reachable from an entry
through its snapshot's representations, verifies their
hash-derived paths and opens read leases which prevent file replacement or
deletion. It then releases the writer. Captures, moves, deletes and collection
can continue while SQLite backs up that exact read view and the exporter
copies the leased files. The staged database drops unreferenced snapshots,
representations and blob rows, then rebuilds its pages with `VACUUM` so old
inline payloads and freed pages do not travel with the export. This process
does not collect or rewrite the source. It checks database integrity, foreign keys, every reachable snapshot’s manifest digest, inline
digests, and lengths and SHA-256 of the copied external bytes. This includes
snapshots referenced exclusively by Floating cards. It installs the
completed staging directory by rename. If a Windows reader briefly prevents
renaming that directory, the install retries for up to one second. A persistent
failure leaves an existing backup and the live store intact.

Desktop startup, search, thumbnail requests, relocation and recovery accept only the ClipRope application ID and schema version above. A different version produces an unsupported-format error. Verification, backup and restore use a read-only source. Restore installs a verified staged copy into a new or empty destination. A format refusal leaves the source history and destination contents intact. [Format policy](../decisions/0028-current-storage-formats.md)

## Reproduction

Use the Release x64 [build commands](../developer/build.md) and run the cases
below. `P1-02.schema_reopen` reopens a store with multiple ropes, representations,
styles and an independent Floating layout and exercises the SQL constraints.
`P1-07.unsupported_format` exercises old versions, an unknown future version and a foreign application ID. `P1-07.backup_install_retry` verifies installation after a transient reader releases the staged directory.
`P1-07.interrupted_writer` kills a separate native process at five boundaries,
then reopens its store and compares entry identities, rope length and actual
blob bytes. It also forces SQLite to return `SQLITE_FULL` by constraining the
disposable database's page quota. The exercise covers process termination.
Hardware power-loss testing is a separate qualification.

The P6 release tests add a real access-denied staging DACL, a missing external
blob, damaged database bytes, interrupted snapshot validation, and recovery
with the headless CLI. The concurrent export test waits after pinning the SQL
and file views, removes the final references to shared and Floating-only
payloads, admits another entry, and runs GC before allowing the export to
finish. It compares restored entry and snapshot IDs, both rope orders,
collector, Floating monitor/rectangle and inline/external payload digests.
`P6-03.reachable_backup_only` leaves deleted entries' snapshots pending GC
and removes one orphan external file. It checks the freshly exported SQL rows,
scans all exported files for the deleted inline sentinel, and restores the
surviving entry and its shared payload.
[Recovery procedure](../developer/maintenance.md#preserving-user-history)

```powershell
pwsh -File tools/build.ps1 -Action Test -TestRegex '^(P6-0[23][.]|ARCH-04[.]sealed_backup|IMG-01[.])'
```
