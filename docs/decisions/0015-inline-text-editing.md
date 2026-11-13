# ADR 0015 · Editing text on the paper

**Accepted.** The desktop owns one lazily created Rich Edit control. The Edit text command, a pencil on a hovered text card and a Floating text double-click bind it to the same saved Unicode representation. A Hanging body double-click in preview retains its [publication contract](0003-native-card-paste.md).

The editor occupies the paper's text inset. Its activatable owned tool window supplies native focus, selection, multiline input, undo and IME composition. The renderer draws the hanger, sheet and Floating header as usual. The native inset uses the paper's fiber tile, text style and opacity. A zero-opacity sheet becomes visible for this session. A moving Hanging card settles over 180 ms. The edited entry stays at its screen anchor through background refresh and resumes normal motion after editing.

| Event | Result |
| --- | --- |
| Ctrl+S or footer check | Show the submitted text immediately. Commit the edit in the background. On failure restore the same native field, selection, undo history and scroll position. |
| Esc or footer cross | Discard this session's changes. An active IME handles its keys first. |
| Paper wheel or drag | Scroll inside the text field or select text. |
| Ordinary focus loss | Keep the draft. Hovered and focused rope navigation stay suspended. |
| Edit another entry, collapse the Hanging workspace, or browse the edited paper out of view | Save first. A rejected write keeps the original view and draft. |
| Floating Return or top close | Bind the return destination or the configured close action at click time. Save first, then return, move to ReBin or delete directly. A failed save retains the draft. A failed return or removal retains the saved Floating preview. [Card deletion](0014-rebin.md) |
| Background eviction | Unbind the evicted identity. Its replacement slot receives no edit. |

**Quick note** and **Ctrl+Shift+Space** save any current edit before creating a new entry. A rejected save keeps the original draft and focus. Each successful invocation commits a visible Floating entry, its blank Unicode snapshot and its layout together, then opens the editor on its paper. The 240 × 160 DIP card is centered in the pointer monitor's work area at invocation time. Canceling the first edit leaves the committed blank paper on the desktop. The collector, rope order, workspace visibility and clipboard remain stable.

A changed save checks the entry ID and expected revision and installs an immutable Unicode-only snapshot. Saving new text on the active head can publish its new snapshot under the [card follow setting](0024-collector-head-clipboard.md). Undoing back to the original paragraphs and canceling preserve the clipboard. An unchanged save retains all rich representations. Undo history belongs to a single edit session. The existing history schema and restart recovery apply to committed edits.

On save or cancellation the owner presents a complete preview frame underneath the native field before hiding it. The paper returns to its configured opacity in that frame. While a save is pending, the owner keeps showing the submitted preview through unrelated repaints. A dependent action keeps its original target and runs once after success. Eviction ends the session and drops any pending action.
