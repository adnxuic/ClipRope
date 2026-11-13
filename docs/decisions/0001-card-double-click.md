# ADR 0001 · Card double-click delivery and history

**Superseded for delivery and sequencing by [ADR 0003](0003-native-card-paste.md).** The original decision and its measurements follow. The [P0 probe results](../../tests/compatibility/delivery.md) cover the native Unicode Edit target on the reference desktop.

## Context

Section 2.1 defines two outcomes for a card double-click. A selected editable text box receives the content immediately. With no selected text box, the user can paste later. Hanging and Floating cards share this input behavior and have different history transitions. Sections 5.2, 7.3 and 17 apply the same rule. [Implementation plan](../../ClipRope_Long_Term_Implementation_Plan.md)

## Decision

1. At the double-click, bind the entry ID and identify the selected editable text box, if any, before the card or workspace can change focus. For a Floating entry, also bind the active rope ID at that moment.
2. Publish the entry's saved formats to the system clipboard and suppress the self-write event. If publication fails, preserve the card's state, order and window, and skip text delivery.
3. After successful publication, deliver the selected card's content once to the captured text box if it still identifies the intended editable target. With no selected box, leave the clipboard ready for the user's next Ctrl+V. If the captured target becomes invalid, skip insertion. A later copy can replace the clipboard contents. Preserve that newer copy and never insert its bytes on behalf of the original gesture. Keep the target transient and avoid a second insertion after an uncertain result.
4. Commit the history action. A Hanging card moves to number 1 of its own rope. The new order governs both numbering and eviction, with the tail evicted first. A Floating card keeps its entry ID, enters the gesture-time active rope at number 1 and participates in that rope's capacity again. Trim any overflow in the same transaction. On success, close the returned Floating window and collapse the workspace if it is open. A workspace already hidden stays hidden.

Steps 3 and 4 have independent outcomes. A failed insertion leaves a successful history commit in place. A failed history commit preserves the prior durable state and window, even if the text was already inserted. Report the actual publication, insertion and history results. The three effects cannot share a transaction.

## Scope and verification

The Floating return button only changes membership and order. An explicit rope-head command or tray selection publishes and promotes through its existing route. Only a card double-click initiates conditional text insertion. The persistent model remains the existing entry, rope and snapshot model.

Validate target capture, loss of focus and supported insertions on Windows with a controlled editable field. Count insertions and verify both card states, no selected field, an invalidated target, a full return rope, commit failures and an already-hidden workspace. The [two-process probe](../../tests/compatibility/delivery.md) records the measured Win32 Edit subset and the handling of lost targets and ambiguous acknowledgements. [Algorithm contracts](../developer/algorithms.md) · [Acceptance cases](../developer/testing.md)
