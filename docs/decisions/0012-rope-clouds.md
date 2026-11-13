# Rope plaques and collector-first overview

Historical schema upgrade procedures in this decision were superseded by [ADR 0028](0028-current-storage-formats.md).

**Accepted.** This decision revises the collector placement in [rope gestures](0010-rope-gestures.md) and [keyboard scopes](0011-keyboard-scopes.md). The original Chinese plan remains a historical source.

Every visible rope has a 144 × 48 DIP wooden plaque above its central sag. The face uses the hanger's wood gradient, fine grain, rounded bevel and two steel rivets. The rope name is centered at 17 DIP and ellipsizes between the rivets. A 12-DIP second line reads `tol:N`, where N counts every rope, including offscreen ropes and hidden ReBin.

The plaque stays centered during horizontal card scrolling and follows its row during dragging. Its 5-DIP corner radius defines both the painted silhouette and pointer target. Clicks outside the rounded corners pass through to the underlying window.

The plaque shares the rope line's menu, drag and overview double-click. Cards retain gesture precedence. A single-view drag saves the rope height; an overview drag previews insertion. Double-clicking a plaque or line in overview selects that rope and opens its single view. The [nine-action menu](0013-rope-menu.md) supersedes the former **Collect on this rope** command in both modes; **Return to this rope** selects the pointed overview row and opens its single view.

Saved rope ranks define a circular switching order. Overview rotates that order to start with the current collector; it does not rewrite ranks on selection. With saved order main, A, B and collector A, overview is A, B, main. The next-rope shortcut selects the next collector, rotates the list and reveals its first row without leaving overview. Ctrl+Tab returns to the complete list's first rope. Scrolling the overview only changes the viewport, including when the collector is offscreen. Existing hovered-row and focused-window input scopes, modifier handling and press ownership remain in force.

An overview drag binds the source, target and current list head by stable IDs. Its transaction resolves the circular order beginning at that head, inserts the source at the previewed target position, saves the resulting ranks and selects the resulting first rope together. A changed collector or missing endpoint rejects the command. Until commit, captures still use the previous collector; cancellation or failure restores the committed layout. Pending capture tickets keep their original destinations. For A, B, main, moving B to the first slot yields B, A, main and collector B. Card order and membership stay stable. The rope switch setting controls publication of B’s head. [Collector head clipboard](0024-collector-head-clipboard.md)

New ropes become collectors only after Yes in the post-creation switch question; No retains the current collector. [Creation decision](0013-rope-menu.md) Deleting the collector falls back to main and rotates overview accordingly. Hidden operation and restart use the last committed collector. Floating return buttons bind that collector when clicked, and explicit drops still bind the pointed rope.

Schema v5 changes the unique main rope's name to lowercase `main`, preserving its ID and all history, settings and placement. Properties make its name read-only; command validation and SQL triggers reject renaming it. Ordinary names remain editable. Existing v1–v4 stores are backed up before the upgrade; failed migrations roll back. Offline backup/restore accepts all supported versions without upgrading the source. [Schema](../schemas/history.md) · [Tasks and evidence](../../Tasks/CLOUD-rope-labels.md)
