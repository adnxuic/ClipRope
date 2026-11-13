# Rope gestures and row scrolling

**Accepted.** A rope press binds the ID of the painted rope line. A short press leaves the collector in place. In overview, a system-recognized double-click on the same line selects that rope for future captures and opens its single view. Right-click opens the menu for the pointed rope. Cards and their controls take precedence over the line.

[Rope clouds](0012-rope-clouds.md) adds the same gestures to each cloud and rotates overview so its collector is first. A committed overview insertion now selects the resulting first rope in the same transaction as its ranks. Scrolling still leaves the collector unchanged.

| Gesture | Single rope | Overview |
| --- | --- | --- |
| Left-drag the line | The rope and its cards follow the first vertical movement. Release saves the new height for this rope. | The gripped line and its cards follow the pointer in front of neighboring rows. Each neighbor moves aside as the dragged row approaches. Release saves the previewed insertion. Holding at an edge scrolls to further rows. |
| Wheel | Browse the current rope's cards. | Browse the cards on the pointed row. |
| Ctrl+wheel | Up selects the preceding collector, down the next. The order wraps. | Browse the vertical list within its limits. All rows that fit remain visible. |

Pressing the left button immediately grips the line. A stationary release leaves its position intact. A small movement within the same overview row still permits a system-recognized double-click. Moving the row to another slot consumes the drag. Escape, capture loss and a rejected write restore the committed layout. Moving a rope preserves its Hanging entries and horizontal browsing anchor. A new collector follows the [clipboard switch setting](0024-collector-head-clipboard.md). The writer resolves both endpoint IDs in one transaction and updates the rope ranks. This order also drives keyboard, menu and wheel navigation.

While the workspace is open, the full width of each displayed row accepts the wheel, including transparent gaps. An editor can keep the caret while the user browses. A click in a transparent gap still reaches the window beneath it. A menu, editor or other window covering the row receives its own input. Fractional wheel packets accumulate for discrete rope changes.

The overview measures insertion against the rows left after removing the carried slot. As the carried row travels one neighbor's height, that neighbor yields one carried-row height in the opposite direction. Halfway across selects the new slot. Reversing the pointer reverses the displacement immediately. The carried rope keeps the vertical grip on its curve through scrolling and retains its visible card viewport until release or cancellation.

Schema v4 stores the single-rope top offset in DIPs from the monitor work area in `rope_view_state.top_dip`. The default is 24 DIP. Switching ropes and reopening the workspace restore the saved value within the available height. Existing profiles retain their card anchors and gain the default offset on migration. [Hanging surface](0003-hanging-card-surface.md) · [Data model](../developer/data-model.md)
