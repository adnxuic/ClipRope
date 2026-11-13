# Algorithm contracts

These contracts define observable results for commands and tests. Display and eviction share one rope order, and Floating entries persist independently. Hanging double-click can send one native paste shortcut to the originally selected editor. [Card double-click decision](../decisions/0003-native-card-paste.md) · [Implementation plan, sections 2–7, 9–10 and 17](../../ClipRope_Long_Term_Implementation_Plan.md)

## Order and capacity

Let `H(r)` be the current list of committed Hanging entry IDs for rope `r`, in ascending `(sort_key, id)` order. The display number is the one-based position in this list. The last item is the first eviction candidate. A transition that admits entries takes a target rope ID and uses the following transaction.

1. Validate the entry, target rope, expected state and required content. Read the authoritative order on the storage writer.
2. Apply the command's change to membership and order. Assign new target keys where necessary. A cross-rope move keeps the entry ID and removes its source membership in this transaction.
3. For a finite rope, retain the first `capacity(r)` items of the resulting target list and delete the remaining suffix. ReBin retains the entire list. Never choose a victim from another rope or from Floating.
4. Commit the new ownership, order, deletions and reference changes together. Only then may views renumber, close a successfully returned Floating window, or present the new state. Storage failure rolls back the whole operation.

With capacity three and initial order `[A, B, C]`, activating C produces `[C, A, B]`. Admitting D then gives `[D, C, A]` and deletes B. Starting again at `[A, B, C]`, dragging A to the tail gives `[B, C, A]`. Admitting D now deletes A. In both cases deletion follows the user's final visible order.

## Command and gesture routing

| Command or action | Success contract |
| --- | --- |
| External capture admission | Admit one new external capture at the frozen target's head and trim. Emit `CaptureCommitted` once the transaction succeeds. Equal bytes in a later genuine copy still create a new entry by default. |
| Hanging card double-click | Publish the saved formats. Move the entry to the head of its own rope, which changes its eviction priority, and collapse the workspace on commit. For a revalidated editor, request one native paste of the published formats. |
| Floating card double-click | Edit text in the card, launch a single file or screenshot, reveal a directory or file group, or show Card information. Keep the clipboard, rope and workspace in their prior state. [Floating actions](../decisions/0008-floating-double-click.md) |
| Expand file group | Bind the group and its membership. Insert one card per saved path after the Hanging source, or replace it in its original slot if retention is off. For a Floating source prepare independent child windows before committing the batch. Commit the entire batch, including any confirmed tail removals, before revealing it. [Card menus](../decisions/0006-card-menus.md) |
| `CopyRopeHeadToClipboard` / tray preview selection | Publish and promote the current Hanging head or the selected tray entry by ID through the existing Hanging activation path. Neither command inserts into a text box. Disable the head action on an empty rope. |
| `FloatEntry` | On a committed drag out, clear the rope and sort key and save the layout of the same entry. The source gains a slot immediately upon commit. |
| Ctrl-drag from a card | At the drag threshold, bind the visible view. In single-rope or hidden mode offer a leased snapshot to the external target with COPY. In visible overview, copy into the indicated slot on any rope or cancel outside. [External drag decision](../decisions/0007-external-drag-copy.md) |
| `RehangEntryToCurrentRope` | The upper return button freezes the active rope at click time, including while tray-resident. Insert at its head, trim and close the Floating window on commit. |
| `RehangEntryToTargetRope` | An explicit drop inserts at the previewed index on the target rope and trims its tail in the same transaction. |
| `ReorderEntry` | Move to position k in the same rope. That position immediately determines eviction priority. |
| Rope line or plaque drag | Save the vertical placement in single view. In overview, insert in the circular order beginning at the bound collector and save ranks plus the resulting first rope as collector atomically. Reject a changed collector or missing endpoint. Commit on release and restore both order and collector on failure. [Rope plaques](../decisions/0012-rope-clouds.md) |
| Hovered or focused Tab | Resolve the next collector in saved circular rope order and commit its selection before refreshing. Overview rotates it to the first row. A failed write retains the previous collector. Ctrl+Tab toggles overview and its complete list's first rope in single view, regardless of vertical scrolling. Each press executes once. [Keyboard scopes](../decisions/0011-keyboard-scopes.md) |
| `MoveEntryToRope` / `CopyEntryToRope` | Moving transfers the same entry atomically. Copying creates an independently mutable entry sharing the immutable snapshot. A pointer drop inserts at the previewed index and trims the target tail. Commands with no specified index insert at the head. |
| `EditText` | Validate entry ID and expected content revision at save. On acceptance, install a new immutable snapshot for this entry. For a plain-text edit of rich content, explain the downgrade before saving and discard stale HTML, RTF, image and Office representations. Skip the write for equivalent paragraphs. [Editing and navigation](../decisions/0015-inline-text-editing.md) |
| Card × | Check the clicked entry and its source rope or Floating membership. Apply the saved choice for that form: erase the entry or move it to ReBin's head. Inside the system ReBin always erase. Retire a Floating window after commit and retain it for retry on failure. [ReBin decision](../decisions/0014-rebin.md) |
| `DeleteEntry` | Direct deletion removes the selected entry and releases its references. A file-reference entry only owns its metadata. |
| `SetRopeCapacity` | Require a capacity of at least one. On shrink, preview the target's excess count. After confirmation, update capacity and trim its current ordered tail atomically. |
| `DeleteRope` | Reject deletion of the main rope. For another rope, explicitly choose deletion of its Hanging entries or a transfer to a specified rope. Preview target overflow. A proposed bulk-transfer order is the source batch in current order followed by the target's old list. Trim the combined tail and commit once. Deleting the active rope selects the main rope for future commands. |

After a manual membership, order, collector or text change commits, apply the [collector head clipboard policy](../decisions/0024-collector-head-clipboard.md). Scroll and selection previews keep the clipboard intact. A Floating return never consults its origin. If a frozen target disappears, preserve the card's prior state and report failure. If a drag is canceled or capture is lost, discard its provisional layout or order. Check the expected revision on edits so an editor cannot resurrect a deleted entry. Whole-card style controls update presentation independently of the content. The context menu uses the saved content type on all three card surfaces. [Card menu decision](../decisions/0006-card-menus.md)

Clear-current-rope and clear-all-ropes operate only on Hanging entries in their stated scope. An explicitly confirmed delete-all-data operation includes Floating. Hiding the workspace, destroying a window on shutdown and restarting do not invoke a delete command.

## External capture

1. On a clipboard update, create a `CaptureTicket` with a command sequence number, the active rope ID, clipboard version and source hint. The capture target is fixed at this point. Apply pause, exclusions, local history restrictions and self-write detection before admission.
2. On an OLE STA outside the UI thread, acquire the source data object, enumerate its representations, select supported formats and request their bytes. Release source-owned resources once serialization finishes. Capture limits cover bytes per format, bytes per entry and queue depth.
   A one-shot process wait owns each worker's deadline. A blocked listener can resume and drain a terminal result after its worker has already been reclaimed.
3. Check source object and clipboard version around acquisition. A version change can reflect delayed rendering. Recheck identity with bounded work and discard uncertain data. Never combine formats from successive copies. Record per-format success, failure or skip. Commit partial content only when at least one usable representation survives and label its actual capabilities.
4. Validate the data, stage and hash bytes, then commit once with the frozen rope's capacity transaction. If the target was removed or all representations failed, drop the uncommitted capture and report a content-free diagnostic. A late completion for a skipped ticket stays discarded.
   After acceptance, one transient writer admits owned snapshots in order on a sibling WAL connection. It reserves at most two slots and 64 MiB of aggregate payload. A refused reservation reports a history failure. The file-install guard excludes GC during admission. The UI connection continues reading committed rows while the writer waits for SQLite.
   Conflicting commands wait in observation order for a terminal result. A deferred Settings write acknowledges success only after commit. A canceled transient draft cannot target a replacement window. A newer external copy cancels a delayed Hanging invocation before its publication. On normal exit, unfinished foreign reads are abandoned, completed snips settle and accepted writes drain before the profile closes.
5. Notify views by stable ID after commit. A matching self-publication creates no entry, promotion or cleanup. Recognize the publication event itself, since comparing content hashes would also suppress a later genuine external copy.

Capture leaves Windows' native clipboard available to the source and target applications. Persist file groups in their original path order. A copied image file path and copied image pixels use different representations. A native cut can still move a file immediately. Later activation of its saved history publishes copy semantics. Treat materialized virtual-file streams separately from path references. Track which formats were saved and which editability claims have been verified. A preview by itself cannot establish restoration fidelity.

The desktop STA owns one ordered queue for admission-dependent commands. It retains up to 64 commands and the latest clipboard observation. Each accepted action keeps its bound identities, sequence and destination while capture finishes. A reentrant drain preserves order. Quit settles accepted writes. End-session cancels pending native actions. The transactional writer reads the actual rope order before applying the domain tail rule. [Queue and reentrancy fixtures](../../Tasks/architecture-evidence/AR2-C.md)

## Clipboard publication, conditional native paste and partial success

Bind the external focus target on the first press of a Hanging card. Verify the entry revision and lease its recoverable formats. A failed publication leaves history, windows and input untouched.

After publishing, attempt the history transaction. Collapse the workspace only on a committed promotion. Then validate the original editor and the exact publication. A failed history transaction allows the same single paste attempt while preserving membership, order and window visibility. A missing, changed or prohibited target leaves the clipboard ready for manual paste. A text-only publication can use the original opaque surface after its native focus and any available field identity are revalidated. For an image file or all-image file group, dispatch once to the bound writable editor or the same paste-enabled Explorer directory. The receiving editor decides how to consume the file list. Browser chrome, Notepad and stock Win32 Edit are excluded. A bound Explorer folder also accepts original files, ordered selections, directories and a PNG stream derived from saved screenshot pixels. Revalidate the same folder and the exact publication while waiting for its Paste command. [Folder paste decision](../decisions/0005-explorer-folder-paste.md) Other non-text snapshots require a verified writable editor. [Image file decision](../decisions/0004-image-file-previews.md) Its paste handler chooses which formats to consume. Record a full shortcut batch as dispatched. Partial dispatch is uncertain and spends the gesture. The consumer can read the clipboard later. [Native paste contract](../decisions/0003-native-card-paste.md)

The published object keeps its content lease through eviction, deletion and editing. On exit flush only the still-owned object and preserve any later clipboard owner.

## View and storage handoff

The [rope menu](../decisions/0013-rope-menu.md) binds its target ID. Back to
first card resets only that rope's anchor. Creation commits a new ID without
changing the collector, then asks whether to select it. Density changes
prepare geometry and commit the per-rope flag and fractional anchor together;
a rejected write leaves the previous live and durable view intact.

A border drag binds the entry and previews its scale in memory. Release commits the scale and any Floating placement together. Canceling restores the original viewport or window. [Border resize](../decisions/0018-card-resizing.md)

The viewport measures each slot from its actual hanger width. The drag projection, scroll anchor and renderer use these advances. [Adaptive image geometry](../decisions/0009-adaptive-image-cards.md)

During an internal card drag, a pointer in the outer 32 DIP of a valid rope row scrolls that row horizontally. Speed increases toward the edge. The stationary pointer continues to select slots as cards pass underneath. Leaving the band or reaching the end saves the row's browsing position. Release uses the current insertion slot. Escape or capture loss discards the insertion preview and retains the browsed position.

The workspace requests visible ropes first and visible Hanging summaries plus a small buffer within each. In single-rope mode only the horizontal window is needed. Keep the reader's viewport anchored by entry ID and local offset across head insertions. When an anchor is removed, choose a nearby surviving card. Rope scrolling and card scrolling leave the active capture rope unchanged. Reject stale ordered pages or preview results by version. Rebuildable caches can shrink independently of retained cards.

Find scopes the immutable text fields to the displayed rope or circular overview. The read worker counts literal, nonoverlapping occurrences in field order. Navigation resolves the stable entry and snapshot again before the workspace reveals the matching row and card. Query changes, scope changes and committed edits invalidate earlier work. A private draft keeps its viewport. [Find in Cards](../decisions/0017-find-in-cards.md)

Staging, blob promotion, SQL commit, orphan collection, backup leases and schema creation have separate recovery boundaries. Their required ordering is recorded in the [data model](data-model.md). Use the [test guide](testing.md) for reproducible domain examples, race checks and failure injection.
