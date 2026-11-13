# ADR 0013 · Rope menu and durable density

Historical schema upgrade procedures in this decision were superseded by [ADR 0028](0028-current-storage-formats.md).

**Accepted · 2026-11-02.** This decision updates section 6.4 of the original
plan and the creation and menu rules in [rope plaques](0012-rope-clouds.md).
The original Chinese plan remains a historical source.

[ADR 0014](0014-rebin.md) adds **Show ReBin / Hide ReBin** below **Next rope** and protects both system ropes.

[Fluent management surfaces](0016-fluent-management.md) adds inline property controls to the same command set.

The rope line and plaque expose the same eleven actions, in this order, in
single view and overview. A menu binds the pointed rope by stable ID.

| Label | Result |
| --- | --- |
| Edit style… | Edit name, color and capacity. The main name remains read-only `main`. Capacity shrink retains affected-tail preview and confirmation. |
| Back to first card | Reset this rope's horizontal anchor to its head. No publication, activation or promotion. Disabled on an empty rope. |
| Compact view / Regular view | Show the available next density and change only this rope. |
| Previous rope | Select the predecessor of the pointed rope in circular saved order. Disabled with one rope. |
| Next rope | Select the successor, with the same wrapping and enabled state. |
| Show / hide ReBin | Include or exclude the system rope from navigation. |
| Show all ropes / Return to this rope | Enter overview, or select the pointed overview row and open its single view. |
| Add new rope… | Save name, color and capacity, then ask whether to switch. |
| Clear Hanging cards… | Preview and confirm removal of this rope's Hanging entries. Keep its rope and independent Floating cards. Disabled on an empty rope. |
| Find in Cards | Open the focused search field for the current browsing mode. [Find behavior](0017-find-in-cards.md) |
| Delete this rope… | Preview and confirm deletion of the rope and its Hanging entries. No transfer selector. Disabled for main. |

The menu no longer offers collector-only selection, default capacity,
clear-all, delete-all, clipboard head recall or keyboard navigation. Other
callers of the underlying history, tray and keyboard commands retain their
contracts. This revision removes menu affordances without removing shared
transaction capabilities.

## Creation and confirmation

`HistoryStore::apply_rope_change` returns an optional created rope ID only
after commit. Creation leaves the active collector unchanged. A separate
modeless Yes/No dialog defaults to No; No, Escape or Close retains the created
rope and the current collector. Yes selects the created ID through the
existing collector command, preserving single/overview mode. Captures during
the question still bind the previous collector.

A failed creation leaves no new row and opens no switch question. A failed
selection leaves the created rope and prior collector intact. The question
reports the failure and permits another Yes or No without repeating creation.
Post-commit display failures are reported separately from write failures.
Deleting the collector still selects main atomically. Canceled, stale or
failed destructive commands preserve the prior committed state.

## Density, geometry and recovery

Regular text paper is 172 DIP wide with a 24-DIP slot gap; compact paper is
86 DIP with a 12-DIP gap. Image body and hanger widths and image body height
are half their regular measurements in compact mode, preserving aspect
ratio. The standard image size supplies the base measurement. Fonts,
controls and Floating presentation retain their own settings.

Viewport layout, painting, hit testing, carried cards and insertion previews
share the measured geometry, including spacing. Crossing ropes during a
drag uses the target's density and maps the grip to its new body geometry.
A density change retains the anchored entry and its fractional slot offset;
the trailing area can remain empty to preserve that anchor. Geometry is
prepared before the density and anchor are committed together. A failed
write leaves both the live viewport and durable state unchanged.

Schema v6 adds `rope_view_state.compact`, constrained to 0 or 1 and defaulting
to 0. Viewports load this per-rope value when opened, including after vertical
virtualization, mode changes, hiding and restart. Existing profiles are backed
up before a transactional v1–v5 upgrade; all ropes initially use regular view.
Read-only backup/verify/restore supports v1–v6 without upgrading its source.

Alternatives rejected: automatic selection on creation, process-only density,
and retaining the transfer choice inside deletion. These do not meet the
accepted interaction and persistence requirements.

[Schema](../schemas/history.md) · [Acceptance and evidence](../../Tasks/ROPE-MENU.md)
