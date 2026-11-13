# ADR 0014 · Card deletion and the ReBin system rope

**Accepted · 2026-11-02. Updated · 2026-11-09.** Cards → General has independent deletion choices for the two card forms. A Hanging × defaults to **Delete directly**. A Floating × defaults to **Move to ReBin**. In either workspace view, deleting an entry already on the system ReBin removes it immediately, without confirmation. This exception uses the reserved rope ID.

Recycling moves the original entry to ReBin's head with its snapshot, style and scale intact. The writer verifies the card's source membership. The active collector, workspace view and ReBin visibility stay in place. Direct deletion releases the entry's content reference. Other references and user files remain available. With an open Floating edit, bind the choice on click and save the draft before applying it. Retire the window only after commit. A rejected edit retains the draft, and a rejected removal leaves the card available for retry. Changes to the current collector's head follow the [card follow setting](0024-collector-head-clipboard.md).

Schema v7 adds the reserved rope ID `system:rebin`. The system rope has a fixed name and SQL NULL capacity. Every other rope has a positive limit. Existing user ropes named ReBin retain their IDs. A ReBin hanger displays its position over ∞. The properties dialog displays read-only **Unlimited**. Users can move cards out, delete individual Hanging cards, or confirm clearing the row. The rope itself survives deletion commands.

Both rope menus offer **Show ReBin** or **Hide ReBin** immediately below **Next rope**. The plaque total includes the system rope at all times. The navigable order includes it only while shown.

| Action | Collector and ordering |
| --- | --- |
| Show from ordinary rope A | Insert ReBin before A in the circular order and select it. It receives external copies and ordinary Floating returns. Overview opens its first row at ReBin. |
| Previous, next or hide in single ReBin view | Return directly to the last ordinary collector. This includes the next-rope shortcut, Ctrl+wheel and a profile with only main and ReBin. |
| Browse a visible ReBin in overview | Select, reorder rows and drag cards as on any other rope. Reordering selects the resulting first row. Hiding removes ReBin and preserves ordinary relative order and the last ordinary collector. |
| Enter single view from overview | Entering ReBin keeps its return destination. Entering an ordinary single view restores ordinary-only cycling. |
| Collapse or restart | Collapse retains this session's selection and visibility. On restart ReBin is hidden and the last ordinary collector receives captures. Deleting that ordinary rope sets the fallback to main. |

The writer keeps the last ordinary collector in persistent settings. A connection-local SQLite table overlays the effective collector and visibility in the same transaction as rank changes. Pending captures keep the destination bound when observed. Every ReBin entry remains reachable through ordinary Hanging ownership during collection, backup and restore.

This revises the Floating close action in sections 2 and 5 of the original plan, and extends the [rope menu](0013-rope-menu.md) and [collector order](0012-rope-clouds.md). [Acceptance](../../Tasks/REBIN.md)
