# Required UI implementation checklist

Read the [UI design contract](../docs/developer/ui-design.md) before editing Windows
UI. This checklist is execution guidance, not a second copy of the design rules.
Apply it to small fixes and every newly introduced component, including transient
windows, expanded states, error feedback and legacy paths that are still callable.

## Intake

- Identify affected component(s), HWND owner, shared adapter and all callers.
- State one observable success, one failure and the transaction/cancel boundary.
- Locate existing fixtures and choose capture, interaction, accessibility and timing checks.
- Identify system-owned exceptions precisely; do not infer an exemption from appearance.

## Completion evidence

Copy this table into the linked acceptance review. Fill every row with observed
evidence or a reasoned **Not applicable**. Use **Open** for unrun/failed checks,
including the exact missing prerequisite and a reproduction command. Do not turn
an open item into Not applicable to close the phase.

| Check | Evidence / reasoned Not applicable / Open |
| --- | --- |
| Shared component, colors, typography, semantic icons, DIP dimensions | |
| Normal, hover, pressed, focused, selected, disabled | |
| Expanded, empty, overflow, pending and error | |
| Mouse, Tab, arrows, Enter, Space, Escape, wheel; IME and shortcuts | |
| Accessible names, states, patterns, result notifications and provider lifetime | |
| English/Chinese, long text, narrow windows, theme changes, four design DPIs | |
| Success, write failure, rollback, cancellation and target invalidation | |
| Timer/subclass/provider disposal, idle repaint, resource recovery and bounds | |
| Before/after captures, opening/closing transitions and separate visual review conclusions | |
| Release first/repeat timing, resource creation and GDI/USER counts | |
| Physical mixed DPI, Windows high contrast, keyboard, IME and screen reader | |
| Scanner, scanner fixtures, component regressions and evidence gate | |

Run the source gate and relevant CTest cases. Put a changed, source-hash-bound record
in [visual evidence](../tests/compatibility/fluent/evidence/README.md). Review the images
before filling the review result. Update the [phase ledger](../Tasks/README.md), state
remaining acceptance work explicitly, and commit each verified implementation phase.
