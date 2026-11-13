# Working on ClipRope

This guide applies to the whole repository. ClipRope is a Windows clipboard history manager. The initial checkout contains the design baseline, development guides and a directory scaffold. Check the current tree before choosing a build or test command.

## Working language

English is the working language of the project. Write new specifications, code identifiers, comments, tests, diagnostics, commit messages, decision records and handoffs in English. Keep the supplied Chinese plan and original brief in their original language so contributors can trace a decision to its source. Answer a contributor in the language they use unless they ask for a different one.

## Starting a task

1. Identify the deliverable, its destination and how to check it from the contributor's current request. A request to document a feature authorizes the documentation work. The implementation stages in the plan describe future work.
2. Consult the [developer guide](docs/developer/README.md) and the relevant sections of the [implementation plan](ClipRope_Long_Term_Implementation_Plan.md). Use [the task workflow](.agent/workflow.md) for source precedence, module selection and change scope.
3. Before changing behavior, spell out one observable result and its failure case. For example, when a Floating card returns to a full rope, record the target, the evicted card and the expected state if the transaction fails.
4. Follow the [verification and handoff guide](.agent/verification-and-handoff.md). Report the evidence obtained for this change and identify any remaining acceptance checks.
5. For any new or changed UI component, follow the [UI design contract](docs/developer/ui-design.md) and complete the [mandatory UI checklist](.agent/ui-checklist.md). Include expanded, error and legacy paths, source-bound visual review evidence and any open physical checks. A successful build or screenshot capture is not visual acceptance.

For implementation work, use the [phase plans and completed-task ledger](Tasks/README.md). Record each verified task there and close each phase with its own Git commit.

Preserve Hanging double-click paste on every change. Run the mandatory regression
gate even for a narrow test selection. Before packaging or publishing, run
`pwsh -File tools/build.ps1 -Action Test -Interactive` on an unlocked Windows test
desktop. The actual Production Release must paste exactly once into the selected
Edge and custom editor fields on first open and reopen. See the [release checks](docs/developer/build.md).

## Building and running

Use `pwsh -File tools/build.ps1` for all builds. One checkout has one CMake tree
(`build/cmake`) and one desktop image (`build/app/ClipRope.exe`). Do not create
task-specific build trees, renamed desktop backups or hard-linked desktop copies.
Production, qualification and Debug replace that output serially. Use `-Action Test`
for the mandatory regression gate and restoration of Production Release, and
`-Action RunDev` for an isolated manual profile. Ordinary builds never update the
daily installation. `-Action Publish` is the separate, explicit installation/update
operation. Preserve user history and the existing sign-in preference. Follow the
[build guide](docs/developer/build.md).

## Boundaries that affect multiple modules

- Each Hanging entry belongs to one rope. Its order determines both its displayed number and its eviction priority. Capacity enforcement removes the final entries on that rope.
- A Floating entry has its own persistent lifetime, with no rope or sort key. The return button binds the current rope at the time of the action. A drop on a rope binds the drop target.
- Hanging double-click publishes the saved formats. The Cards preference selects native paste or copy only. In native paste mode send one Ctrl+V after the history attempt when the original editor is still valid. A text-only publication can also use the original opaque surface after revalidating its native focus and any bound field identity. Image file references use the bound editor's native paste handler or the verified Explorer folder under the [image file decision](docs/decisions/0004-image-file-previews.md). A complete send reports shortcut dispatch. A bound Explorer folder can also receive real files, groups, directories or a screenshot PNG under the [folder paste decision](docs/decisions/0005-explorer-folder-paste.md). With no bound destination, leave the content available for the user's next Ctrl+V. Track clipboard publication, text delivery and history commit separately.
- On successful Hanging double-click, promote the card to its own rope's head. Trim that rope as needed and collapse the workspace if open. By default, manual changes to the current collector or its head publish the resulting head for manual paste after commit. Scrolling alone preserves the clipboard. See the [collector head decision](docs/decisions/0024-collector-head-clipboard.md). See the [card double-click decision](docs/decisions/0003-native-card-paste.md).
- The card × applies the Cards preference for its current form. Hanging defaults to direct deletion and Floating defaults to moving the same entry to the head of unlimited ReBin. Inside the system ReBin, × always deletes directly. Close a Floating window only after commit. [ReBin behavior](docs/decisions/0014-rebin.md)
- A Floating body double-click edits text in place, opens a file or screenshot, reveals a folder or file group, or shows saved information. Clipboard and rope order remain stable. See the [Floating double-click decision](docs/decisions/0008-floating-double-click.md).
- Views issue application commands. Domain rules use stable identities and remain independent of Win32, COM, graphics and SQLite. Check the [architecture](docs/developer/architecture.md) and [algorithm contracts](docs/developer/algorithms.md) before moving behavior between layers.

Use these boundaries to find affected callers and tests. The [data model](docs/developer/data-model.md) defines ownership and the lifetime of shared data.
