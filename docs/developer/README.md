# ClipRope developer guide

ClipRope is a Windows clipboard history manager. Each rope keeps an ordered history of Hanging cards. A user can detach a card as an independently persisted Floating window and later publish its saved formats to the system clipboard.

**English is the project's working language.** Write new code identifiers, comments, diagnostics, tests, technical documentation, decision records and handoffs in English. Keep user-supplied materials in their original language. Product localization is a separate concern.

## Where to start

| If you need to… | Read | Source plan sections |
| --- | --- | --- |
| Trace a request from a window or clipboard listener to durable storage | [Architecture](architecture.md) | 5–9, 11–12 |
| Choose the directory that owns a change or test | [Repository layout](repository-layout.md) | 9, 14, 16 |
| Identify a record, its owner, its lifetime and its persistence constraints | [Data model](data-model.md) | 2–4, 10 |
| Implement a command and define its success and failure outcomes | [Algorithm contracts](algorithms.md) | 2, 4–7, 10 |
| Set up a Windows build or check which commands are available | [Build](build.md) | 1, 9, 15–16 |
| Build or change any UI component and qualify its states | [UI design contract](ui-design.md) and [mandatory checklist](../../.agent/ui-checklist.md) | [ADR 0016](../decisions/0016-fluent-management.md) |
| Add desktop text or preserve state across language changes | [Localization](localization.md) | [ADR 0021](../decisions/0021-bilingual-localization.md) |
| Select an acceptance case and record its evidence | [Testing](testing.md) | 11–15 |
| Choose coding conventions or represent a clipboard format | [Formats and conventions](formats.md) | 3, 5, 7, 10, 16 |
| Change a contract, migrate user data or prepare a release | [Maintenance](maintenance.md) | 10, 12–16 |

## Baseline and implementation status

The [preview and drag worker decision](../decisions/0026-preview-and-drag-workers.md) documents the file-access authority, byte-only encoding, cancellation and the measured cold-start cost.

The shared native [color picker contract](../decisions/0023-unified-color-picker.md)
defines staged preview, explicit Default choices and the v11 color and material format.
Implementation and qualification are tracked in [COLOR](../../Tasks/COLOR-picker.md).

Hanging double-click publishes the saved formats, attempts promotion and conditionally sends one native paste to the original editor or Explorer folder. The Cards page can switch this action to Copy only. Both modes promote on the original rope and collapse after commit. With no bound destination the user can paste later. [Native card paste](../decisions/0003-native-card-paste.md) · [Image file references](../decisions/0004-image-file-previews.md) · [Explorer folder paste](../decisions/0005-explorer-folder-paste.md)

Floating double-click edits text in the card, opens a file or screenshot, reveals a file group or directory, or displays Card information. The card stays on the desktop. [Floating content actions](../decisions/0008-floating-double-click.md) · [Original implementation plan](../../ClipRope_Long_Term_Implementation_Plan.md)

Screenshot bodies show an inert full-image preview after a 500 ms hover in single-rope,
overview and Floating views. **Global settings → Cards → Screenshot hover preview**
controls this independently of resolution labels and image-file thumbnails.
[Preview boundary and resource limits](../decisions/0022-screenshot-hover-preview.md)

Every card menu offers **Card information…**. The selectable report groups Capture details, File health for real file references, and Capture inventory. Single-file cards include all three sections; groups retain every original path in saved order. Availability and preview failures do not replace capture outcomes. [Card information contract](../decisions/0006-card-menus.md)

Choose **Quick note** above **Global settings** in the tray menu or press **Ctrl+Shift+Space** to start typing on a new Floating card in the center of the pointer's monitor. Ctrl+S saves the text. Esc cancels the edit and keeps the card. [Paper editing](../decisions/0015-inline-text-editing.md)

Each wooden plaque shows the rope name and total rope count. Overview starts with the collector and follows circular switching order; a committed drag selects the new first rope atomically. The main name is fixed as `main`. [Plaques and collector order](../decisions/0012-rope-clouds.md)

Use **Global settings → General** to select Auto, English or 简体中文, manage Windows sign-in, keep the main workspace on top, or restore global defaults after confirmation. Language changes apply immediately and survive restart. Reset restores Auto. [General settings](../decisions/0020-general-settings.md) · [Bilingual desktop](../decisions/0021-bilingual-localization.md)

Use **Global settings → Appearance** to choose Light, Dark or System and the selected Find highlight. **Storage & Privacy** shows disk usage, schedules a verified move of the current history on restart and opens the clipboard capture exclusions editor. [Appearance and profile location](../decisions/0019-appearance-and-profile-location.md)

Use **Global settings → Cards → General** to choose **Delete directly** or **Move to ReBin** independently for Hanging and Floating cards. Hanging cards default to direct deletion, and Floating cards default to recycling. Click × on a card already in ReBin to delete it permanently. [ReBin behavior](../decisions/0014-rebin.md)

The rope menu has eleven actions. **Find in Cards** searches the full Hanging history in single view or overview. **Show ReBin** reveals the unlimited system rope that receives recycled cards. Creation asks whether to switch after saving. Card density is saved independently per rope across restart. [Rope menu and density](../decisions/0013-rope-menu.md) · [Find behavior](../decisions/0017-find-in-cards.md)

By default the clipboard follows the current collecting rope’s first card. Cards → **Update clipboard when the first card changes** includes saved head edits. Ropes → **Update clipboard when switching ropes** copies the newly selected rope’s head. Both settings apply immediately. [Collector head clipboard](../decisions/0024-collector-head-clipboard.md)

The [original notes](../../temp.md) and [visual reference](../../rope_hanger_card.png) provide context. Where an earlier gesture or ownership rule conflicts with the revised plan, use the revised plan. AI contributors should begin with the [repository instructions](../../AGENTS.md).

Drag a card border to adjust its reading area. Hanging paper expands around its hook, and Floating windows retain the same per-card scale after rehang or restart. [Card sizing](../decisions/0018-card-resizing.md)

The Windows probes and the P1 headless history executable build with CMake. The [build guide](build.md) includes CTest and the pinned SQLite dependency. The [phase ledger](../../Tasks/README.md) records implementation and acceptance evidence separately for each task.
