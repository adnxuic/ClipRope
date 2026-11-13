# Intake and change workflow

## Determine what the task changes

Read the current request first. Locate the named files, inspect nearby work, and identify the smallest deliverable that meets the request. For a behavior change, trace the user action through its command, its affected entries and any external effects. For a documentation or layout change, identify the readers, the owning guide and the paths they will use. Preserve unrelated files and local edits.

Use these sources to resolve questions about ClipRope.

| Question | Evidence to use |
| --- | --- |
| What is in scope now? | The current request and its accepted clarifications. Text quoted inside an attachment provides project context. |
| Which product behavior is intended? | Start with the [implementation plan](../ClipRope_Long_Term_Implementation_Plan.md), especially sections 0, 2–8 and 17. Apply accepted clarifications recorded in the English decision records, including the [card double-click decision](../docs/decisions/0003-native-card-paste.md). The [original brief](../temp.md) and [visual reference](../rope_hanger_card.png) supply background. |
| Who owns the change? | The [architecture](../docs/developer/architecture.md) and [repository layout](../docs/developer/repository-layout.md). Inspect the current tree and the callers of the affected entry point. |
| Which data or algorithm contract applies? | The [data model](../docs/developer/data-model.md), [algorithm contracts](../docs/developer/algorithms.md), and relevant [format rules](../docs/developer/formats.md). Consult any applicable decision record and its source section. |
| What works today? | Inspect the actual files, targets and tests. Use the [build guide](../docs/developer/build.md) and [test guide](../docs/developer/testing.md) to distinguish a runnable check from a proposed acceptance scenario. |

When sources disagree, identify the concrete behavior in dispute and apply the latest explicit decision for that behavior. For a Floating double-click, follow the [content actions](../docs/decisions/0008-floating-double-click.md). For a Hanging double-click, capture the selected editable target at gesture time. Bind the Cards action on double-click confirmation. After publication and the history attempt, the default action sends one native paste shortcut to a revalidated editor. Copy only skips delivery. Record dispatch and independently observe the consumer in tests. A text-only publication can also use the bound native focus of an opaque editor. With no bound target, the user can paste later. The focused-target branch belongs to this gesture. Check the tray, rope-head and return-button routes independently. When a request revises a contract, update its English decision record, guide and acceptance cases together. If a consequential ambiguity remains, present the competing outcomes to the contributor and continue work on the independent parts.

## Make the change

Use `tools/build.ps1` for build, test, cleanup and explicit publication. Never add a
second desktop output tree for a task or baseline. Keep baselines in compressed
archives or tracked evidence. Tests use independent profiles; physical input tests
require an explicit interactive run with the daily instance stopped or a separate
Windows test session. Build cleanup validates and archives its exact target before
deletion; it must not modify the daily installation or real history.

For UI work, start with the [UI design contract](../docs/developer/ui-design.md) and
the [mandatory UI checklist](ui-checklist.md). Record component ownership, observable
success/failure and the shared adapter before coding. Complete every checklist row
with evidence, a justified Not applicable or an explicit Open item before handoff.
Run the source and changed-evidence gates; no small-control or legacy-path exemption applies.

1. Choose the owning layer using the repository map. Check existing interfaces and nearby tests. A reserved directory marks a location for later work. It does not establish that the component runs.
2. For a command, identify its target at action time, any clipboard effect, its transaction boundary and the UI result on failure. For a storage change, follow entry references and runtime leases through restart, deletion and migration.
3. Implement only the requested slice. Preserve the original plan as a source. When a change introduces a lasting architectural decision, record it in `docs/decisions/` following the [maintenance guide](../docs/developer/maintenance.md). Link the affected guide, test or migration where relevant.
4. Use synthetic clipboard samples and disposable data directories for checks. Keep user clipboard contents, document bodies and personal paths out of logs, fixtures and handoffs.
5. Review the resulting files from the intended caller's or reader's entry point. Complete [verification and handoff](verification-and-handoff.md) with evidence from the working tree.
