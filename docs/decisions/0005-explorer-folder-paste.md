# ADR 0005 · Paste a card into an Explorer folder

The paste gesture in this record applies to Hanging cards. Floating cards now use [content-specific double-clicks](0008-floating-double-click.md).

**Accepted.** On a card double-click, the file pane that had focus before the first press can receive a native paste. ClipRope binds the filesystem directory and tab through the focused Shell view. This applies to Hanging and Floating cards.

| Card | Folder paste result |
| --- | --- |
| A real file or Windows shortcut | Copy the original filesystem item with its filename. A selected `.lnk` remains a shortcut. |
| A real folder or a multi-selection | Publish the original ordered `CF_HDROP` paths with the preferred copy effect. Explorer copies directories recursively and handles naming conflicts. Every member must still be available before publication. |
| Saved image pixels | Offer one `Screenshot-YYYYMMDD-HHMMSS-mmm.png`, named with local activation time. Reuse a saved PNG byte for byte. Otherwise encode the original DIBV5 or DIB at full resolution and preserve represented alpha. |

The screenshot's Shell descriptor and indexed file stream exist only in the publication created for the bound folder. The data object also retains the saved pixel formats. Each file-stream request starts at offset zero. The file presentation is generated after validating the saved image. Capture limits for third-party virtual files still apply to incoming clipboard data. The normal exit flush keeps the portable pixel formats. Reopening the card recreates its folder presentation. A rich snapshot containing text alongside an image retains its ordinary restore behavior. Text paths and web URLs do not become filesystem items. [Shell clipboard formats](https://learn.microsoft.com/en-us/windows/win32/shell/clipboard)

Publication precedes the usual promotion or rehang transaction. After the history attempt, allow the Explorer command bar a short bounded interval to reflect the new clipboard. While servicing clipboard requests, recheck the foreground file pane, directory and publication. Dispatch one Ctrl+V when Paste is enabled. If the command bar stays stale, query the bound folder's transfer handler with the authenticated clipboard object. A positive COPY effect also permits dispatch. End that capability check with DragLeave. A changed target, replaced publication or unavailable copy operation leaves the gesture without input. Record dispatch separately from the files subsequently observed in the folder. [Native paste decision](0003-native-card-paste.md) · [File reference contract](../schemas/file-groups.md)
