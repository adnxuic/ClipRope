# ADR 0006 · Card context menus

**Accepted.** The [Fluent management surfaces](0016-fluent-management.md) decision updates how style controls are presented. Hanging, Floating and overview cards use the same commands in display order.

| Saved content | Menu |
| --- | --- |
| Editable Unicode, including URLs and text with rich companions | Font size…, Bold, Text color…, Card background color…, Opacity…, Edit text…, Card information… |
| Screenshot pixels | Open original file, Show in File Explorer, Card information… |
| One real file, including an image file | Open original file, Show in File Explorer, Refresh file preview, Card information… |
| Multiple real paths | Show in File Explorer, Refresh file preview, Expand file group, Card information… |
| One real directory | Show in File Explorer, Refresh file preview, Card information… |
| Other content, including virtual files | Card information… |

**Updated · 2026-11-04.** Every content type exposes one **Card information…** command. Its window is titled **Card information** and presents selectable text in this order:

| Section | Fields | Applies to |
| --- | --- | --- |
| Capture details | Saved (local capture time), Source (application or Unknown), Capture (saved capture outcome) | Every card |
| File health | Original path, File status, Preview | Real files, directories and file groups, including image file references |
| Capture inventory | Every recorded format with its saved outcome and index | Every card |

A group lists all original paths in saved order, including repeated members, followed by the existing aggregate health and preview state. Pixel screenshots and virtual files have no File health section. File information is available before background path classification finishes. Opening it performs no synchronous external path probe. Background completion refreshes the same information window while retaining the selection and scroll position. Missing or inaccessible original files and failed image decoding do not suppress capture metadata or the format inventory. Capture, file availability and preview decoding remain distinct outcomes. No-preview files report Preview: Not applicable. The structured window and plain-text report share the same rows. A deleted or replaced snapshot reports that the card is unavailable. Viewing information leaves clipboard publication, membership and history order unchanged. File-opening errors retain the separate File information diagnostic.

Style edits apply to one entry. Opening, revealing and refreshing bind the snapshot by identity and leave clipboard publication and history ordering to their existing gestures. A directory is selected in its parent. A multi-selection in one parent is selected together. Across parents, the user chooses one of the represented directories.

**Expand file group** normally keeps the group and creates one independent card for each saved path, including repeated and missing members. Each child retains the original capture time and source and offers its own path with the copy drop effect. A locally identified directory keeps its folder type if it later disappears. Expansion preserves the system clipboard. Invoking the action again creates another batch.

On a rope, the children follow the group in its saved file order. If capacity would remove existing tail cards, the Fluent confirmation names the rope and shows how many cards will be created and removed. **Expand group** confirms the removal. **Cancel** receives initial keyboard focus. Recheck the tail identities at commit. If any child would fall outside capacity, the capacity notice shows the minimum needed at this position and offers **OK**. ReBin can accept the whole batch.

When **Keep original group after expansion** is off, the children replace the group in its original slot. Calculate the capacity preview with that slot freed. Remove the source entry in the same transaction as the children and any approved evictions. Retire a Floating source window only after commit. The files on disk are never removed by expansion.

On the desktop, the children become independent Floating windows on the group's monitor. Place them in reading order in the available work area and stagger the remainder. Prepare the windows hidden, persist all layouts with the entries, then show them after commit. A failed preparation or transaction discards the batch. [File reference format](../schemas/file-groups.md)

On a Hanging text card, **Edit text…** focuses a multiline editor inside the selected sheet in either workspace view. Save or Ctrl+S commits changes to that entry, Enter adds a line, and Cancel or Esc discards the draft. Unchanged text keeps its saved formats. A failed save leaves the text and diagnostic in the card. Scrolling the edited text out of view or hiding the workspace saves the draft before navigation. A rejected save keeps the editor visible. [Inline editing](0015-inline-text-editing.md) Removing a card from its rope closes its editor. [Hanging double-click paste](0003-native-card-paste.md) · [Floating inline editing](0008-floating-double-click.md)

A screenshot uses its explicitly supplied source path when available. For a system snip, ClipRope records the resolved Windows Screenshots folder even when it missed the foreground overlay. For an observed capture round it can bind a unique newly created PNG in that interval after matching its full dimensions and straight-alpha pixels. Older Snipping Tool cards without a recorded directory resolve the current Windows Screenshots known folder. Without a round, selection requires a byte-identical PNG created within three seconds of the stored capture second and a unique match among nearby captures. A known original path remains the target if the file disappears.

**Show in File Explorer** selects the verified original. If the file cannot be identified but the system screenshot folder is known, it opens that folder without selecting a file. **Open original file** can materialize a full-resolution PNG for an unbound screenshot. Pixel cards from sources with no known screenshot folder also use the managed PNG for reveal. Copies sharing a snapshot reuse the file. Collection reclaims the managed file after its last entry disappears. [Folder paste decision](0005-explorer-folder-paste.md)

Refresh checks all referenced paths on the preview worker. It distinguishes a missing item, a temporarily inaccessible item and a readable file whose image preview fails to decode. When available, a prior thumbnail can identify a missing file. Saved formats and capture completeness are shown separately from filesystem availability.
