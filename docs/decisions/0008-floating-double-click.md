# ADR 0008 · Double-clicking a Floating card

**Accepted.** A left double-click on a Floating card's body acts on its saved content. The gesture preserves the clipboard, rope order, card position and workspace visibility. The press binds the entry identity and revision. The desktop checks them again when Windows confirms the double-click. [Hanging paste](0003-native-card-paste.md) · [File and screenshot commands](0006-card-menus.md)

| Content | Action |
| --- | --- |
| Editable text, including URLs and text with rich companions | Focus a multiline Unicode edit control in the card. |
| Single file or screenshot | Open the original with its Windows association. A screenshot without a bound original uses its full-resolution managed PNG. A missing bound original reports an error. |
| File group | Open its parent in Explorer and select its members. For multiple parents, offer the represented folders and select the members of the chosen one. Cancel dismisses the chooser. |
| Directory | Select it in its parent. Open a filesystem root directly. |
| Other, including virtual attachments | Show the unified Card information window. |

The paper footer provides Save and Cancel. The Floating header remains available during editing. Either header action saves the draft before applying its ownership command. Enter inserts a newline, Ctrl+S saves and Esc cancels. Switching focus keeps the draft in memory. The revision check and recovery rules are shared with Hanging cards. [Inline editing](0015-inline-text-editing.md)

Hanging activation continues to publish, promote and conditionally paste. The Floating return button and explicit drop retain their own ownership commands. This change uses existing profiles.
