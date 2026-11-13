# ADR 0004 · Image files on cards

The paste gesture in this record applies to Hanging cards. Floating cards now use [content-specific double-clicks](0008-floating-double-click.md).

**Accepted.** This decision extends the file-reference contract in sections 7 and 11 of the source plan and defines automatic paste for image file references.

A saved real file list containing only PNG, JPEG or BMP names occupies one entry. Capture reads that list before requesting optional pixel or virtual-stream companions. Recall publishes the ordered paths and the preferred copy effect. The same rule applies to older snapshots with a saved pixel companion. A copied screenshot carries its saved pixel formats. A double-click bound to an Explorer folder also offers a PNG file as specified in the [folder paste decision](0005-explorer-folder-paste.md). Classification uses the existing snapshot manifest and requires no database migration.

A single image file uses an adaptive image body and exposes its filename on hover under [ADR 0009](0009-adaptive-image-cards.md). An image group shows up to four members in capture order and its full count. Both retain the file glyph. The workspace, overview, carried paper and Floating sheet use the same renderer. A mixed group keeps its file-list template.

The supervised thumbnail helper verifies the live stored descriptor before opening its paths on demand. [Worker roles](0026-preview-and-drag-workers.md) Its cache identifies the ordered list by its descriptor hash, records the sizes and modification times of the pictured members, and separates card-size variants. Decode failure leaves the file card usable. Losing a member blocks publication of the entire group. A previously built preview can remain visible with a missing or inaccessible label. Refresh file preview, device changes and subsequent views recheck the original paths. Network and reparse-point paths require a user-initiated refresh for a source read. Scrolling can still show their cached preview. GC reclaims derived previews when the final history reference disappears.

A double-click publishes first and attempts the normal promotion or rehang transaction. A bound writable editor receives one native Ctrl+V and handles the original file list according to its ordinary paste behavior. This includes web composers, custom UI Automation editors, Office document selections and Rich Edit controls with application-defined paste handlers. Explorer requires the same bound directory and a verified Shell paste destination. The host revalidates focus, permissions and the exact clipboard receipt before dispatch.

For Chromium and Firefox windows, the bound UI Automation editor must be in the page's document subtree. Browser chrome, Explorer text fields, Notepad, stock Win32 Edit, password fields and read-only controls leave the clipboard available for manual paste. A target change cancels dispatch. Clipboard publication, history commit, shortcut dispatch and observed attachment consumption are distinct outcomes. [Shell file formats](https://learn.microsoft.com/en-us/windows/win32/shell/clipboard)

**Open original file** invokes the default file handler. **Show in File Explorer** selects the original. **Refresh file preview** checks the saved references and rebuilds the picture or collage. The [card menu decision](0006-card-menus.md) supersedes the original pixel-copy command.

[File-group format](../schemas/file-groups.md) · [Qualification](../../Tasks/IMG-image-files.md)
