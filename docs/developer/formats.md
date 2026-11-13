# Format conventions

English is the project working language. Use English for identifiers, comments, documentation, tests, diagnostics, change summaries and handoffs. Preserve user clipboard bytes, registered format names, original-language reference files, names and localized strings in their original form. Keep translatable UI strings in resources as the UI is implemented.

## Source and documentation

- Write new text files as UTF-8. Use Markdown with relative, repository-local links for developer documents. Keep code, shell commands and machine identifiers in code spans or fenced blocks. Mention the relevant numbered plan section when documenting a contract or its acceptance gate.
- Use the module names and paths from the [repository layout](repository-layout.md). Keep C++ type and command names consistent with `EntryId`, `ContentSnapshot` and `ReorderEntry`. Use `snake_case` for persisted field names such as `rope_id` and `sort_key`. Use the exact state names `Hanging` and `Floating` when discussing the domain. Capitalization matters in serialized identifiers.
- For new C++ files, pair `.hpp` interfaces with `.cpp` implementations where both are needed. Use four spaces per indentation level, make ownership visible at API boundaries and use RAII with the matching type-specific release operation. In particular, honor the ownership and medium of a `STGMEDIUM` when calling `ReleaseStgMedium`. Keep necessary platform headers in their adapters.
- Follow existing local style when modifying a file and keep formatting edits within the change. A pinned formatter/configuration and automated style checks are pending. When adopted, record the formatter version, configuration path and invocation in [build instructions](build.md).

## Clipboard representations

A persisted snapshot describes what can actually be published after source exit and application restart. A preview or a format being offered by the source is insufficient evidence of a recoverable representation. Keep a manifest of adapter outcomes. For each supported representation record its standard clipboard ID or exact registered format name, `TYMED`, `dwAspect`, `lindex`, codec version, status, and size/content reference for retained bytes. Re-register custom names on restore. Keep process-local handles and COM pointers within the owning adapter's lifetime.

| Family | Capture and restore contract |
| --- | --- |
| Unicode text | Preserve recoverable character content. Keep search keys and display excerpts separate from the payload. |
| HTML and RTF | Validate format lengths, offsets and encoding as applicable. Persist supported original bytes and advertise only recoverable formats. Render previews without executing scripts or fetching remote content. |
| Image pixels | Keep the supported original or a documented, versioned, recoverable image representation. Validate dimensions and byte arithmetic. Store thumbnails as disposable cache entries. |
| File references | Persist the ordered path list and necessary metadata. One file group consumes one entry. A `.png` copied as a file stays a file reference. Verify availability on restore. Preserve native cut behavior on initial capture and offer copy semantics on history restore. |
| Virtual files | Handle descriptors, indexed content streams and drop effects explicitly. Claim durable content only for streams the adapter serialized successfully. |
| Office/private objects | Skip unverified private formats by default. Retain supported alternatives such as text/HTML/image with an accurate capability label. Require scenario-specific compatibility evidence to advertise native editability. |

Show a partial outcome when some chosen representations fail and the rest are usable. A fully failed capture cannot be presented as restorable. Whole-card styling remains presentation metadata. On an explicit plain-text edit of rich content, disclose the downgrade before saving and build a fresh snapshot containing only formats consistent with the edited text.

## Durable formats and version changes

The [data model](data-model.md) owns entity identities, SQLite constraints, Blob lifetime and layout. The [algorithm contracts](algorithms.md) define commit and publication order. Before adding a persisted field or codec, record its name, type/units, ownership, nullability, version and verification rules in `docs/schemas/`. Check creation, reopen and rejection of unsupported data against the [current storage contract](../decisions/0028-current-storage-formats.md).

Address immutable Blob bytes by strong content hash and length. Store external Blobs under controlled relative paths. User file references keep their original paths and ownership. Persist window geometry using sizes in device-independent pixels, monitor identity and a position relative to the work area. Keep user content and full paths out of diagnostic records and compatibility reports. Export manifests must cover the consistent database snapshot and referenced Blobs for both entry states. See [maintenance](maintenance.md).

These conventions develop the [implementation plan, sections 3, 7, 9, 10, 11 and 16](../../ClipRope_Long_Term_Implementation_Plan.md).
