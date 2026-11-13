# Verification and handoff

## Choose checks for the change

| Change | Evidence to collect |
| --- | --- |
| Documentation or scaffold | Follow the relative links from the changed guide. Verify every claimed current directory and file exists, including dot-prefixed paths. Compare terms and examples with the relevant plan sections. Label proposed targets and commands clearly. |
| Domain or application | Exercise the affected command through its normal entry point. Check identity, ownership and final order for success and failure. Include the smallest example that distinguishes the new behavior from a regression. |
| Storage or a durable format | Use a disposable store. Check rollback and reopening. If upgrading records, exercise a representative older fixture, including independent Floating entries and shared data where applicable. |
| Windows integration | Use a controlled source or test window for clipboard and input checks. Record the Windows environment and actual observed result. Office fidelity, cross-process hit testing and DPI behavior need the relevant host scenario. |
| UI appearance or layout | Record first-open and repeat-open Release latency, resource creations and reference images for the affected interaction. Use the [menu benchmark](../tests/performance/README.md#context-menu-latency) for popup changes. |
| Build or performance | Use commands backed by checked-in build files. Record configuration, toolchain and result. For a measurement include the workload, reference machine, and processes included in the metric. |

Check the [build instructions](../docs/developer/build.md) and [acceptance cases](../docs/developer/testing.md) for details. At the scaffold stage, verify the files directly. For example, from the repository root these PowerShell lines check representative navigation targets and list the contents of the AI workflow directory.

```powershell
Test-Path -LiteralPath 'AGENTS.md' -PathType Leaf
Test-Path -LiteralPath 'docs/developer/README.md' -PathType Leaf
Get-ChildItem -Force -LiteralPath '.agent'
```

For a new link, resolve its destination relative to the containing document and check its anchor when present. For a new folder, compare its location with the [repository layout](../docs/developer/repository-layout.md). Run build or test commands when their targets exist. Record the exact missing prerequisite if a check cannot run. A directory check supplies evidence about the scaffold only.

## Handoff

UI handoffs must include the completed [UI checklist](ui-checklist.md), a source-bound
[visual acceptance record](../tests/compatibility/fluent/evidence/README.md), and a
review against the [design contract](../docs/developer/ui-design.md). Capture success
and visual acceptance are separate results. List physical DPI, Windows high contrast,
keyboard/IME, screen-reader and latency checks that remain open, with prerequisites.

Use an English handoff in the repository or for the next contributor. Keep the user-facing reply in the conversation's language. Include the fields that apply.

| Field | What to record |
| --- | --- |
| Outcome | The requested deliverable, the paths or command that lead to it, and the observable result. |
| Decisions | The relevant plan section or decision record, any assumption made, and its effect on the implementation or guide. |
| Verification | The command or manual steps actually performed, the relevant environment, and the observed result. Identify failures and checks that remain unrun separately. |
| Continuation | Any concrete remaining acceptance case or open decision, the prerequisite it needs, and the file or test where work resumes. |

Keep evidence proportionate. A documentation change can close with the files and links checked. A clipboard compatibility claim requires the exact source application, format list, restore path and observed outcome.
