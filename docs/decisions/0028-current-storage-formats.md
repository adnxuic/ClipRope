# ADR 0028 · Current persistence formats

Accepted on 2026-11-11. The earlier databases were disposable test profiles. The product now creates v11 directly and accepts that version for opening, verifying, exporting, restoring and relocating history. This supersedes the upgrade procedures in section 10.4 of the original implementation plan. An unsupported database produces a format error and retains its original contents. Current-format backups still use a frozen SQLite snapshot and leases on reachable blobs. [History schema](../schemas/history.md)

Every committed snapshot carries a SHA-256 digest covering its ordered representations. Loading a snapshot or verifying an export with an absent or incorrect digest fails. Capture and edit write their search fields transactionally. Shortcut overrides use the per-action table. Each optional preference has a defined initial value. [Content and publication leases](../schemas/content.md)

Pixel thumbnails use THM3 and file thumbnails use CRF2. A cache miss for an available source decodes it and stores the result in the current format. An unavailable source can use a current cached preview. If only obsolete cache bytes exist, its card shows the ordinary unavailable-file state. The Windows clipboard adapters retain their existing representation contracts. [File references](../schemas/file-groups.md)
